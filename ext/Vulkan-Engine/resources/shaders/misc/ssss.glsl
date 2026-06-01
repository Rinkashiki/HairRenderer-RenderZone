#shader vertex
#version 460

layout(location = 0) in vec3 pos;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec2 v_uv;

void main() {
    gl_Position = vec4(pos, 1.0);
    v_uv        = uv;
}

// ----------------------------------------------------------------------------
#shader fragment
#version 460

layout(location = 0) in vec2 v_uv;

// --- Input textures (all from ForwardPass MRTs + SSAO pass) -----------------
layout(set = 0, binding = 0) uniform sampler2D hdrTex;        // full HDR shading
layout(set = 0, binding = 1) uniform sampler2D albedoMaskTex; // RGB = albedo, A = scatterMask
layout(set = 0, binding = 2) uniform sampler2D diffuseIrrTex; // front diffuse irr (no albedo)
layout(set = 0, binding = 3) uniform sampler2D backIrrTex;    // back diffuse irr (flipped N)
layout(set = 0, binding = 4) uniform sampler2D depthTex;      // R = gl_FragCoord.z
layout(set = 0, binding = 5) uniform sampler2D aoThickTex;    // R = AO, G = thickness
layout(set = 0, binding = 7) uniform sampler2D brightTex;     // Bright pass-through for Bloom
layout(set = 0, binding = 8) uniform sampler2D scatterDistLUT; // 5-pixel LUT: thin→thick (sRGB)

// --- Uniform block (std140, binding 6) --------------------------------------
layout(set = 0, binding = 6) uniform SSSBlock {
    // xy = (theta, r), zw = unused (vec4 for std140 array padding)
    vec4  samples[64];

    int   sampleCount;
    float maxScatter;       // world-space scatter radius scale
    float extinctionCoeff;  // Beer-Lambert extinction coefficient
    float Fdr;              // internal Fresnel diffuse reflectance

    vec2  screenSize;

    layout(offset = 1056) mat4 projection;
    mat4 invProjection;
} sss;

// --- Outputs ----------------------------------------------------------------
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outBright; // pass-through to Bloom

// ----------------------------------------------------------------------------
const float PI  = 3.14159265358979323846;
const float EPS = 1e-6;

// Sample one of the 5 LUT pixels (sRGB → linear)
vec3 sampleProfile(int i) {
    float u = (float(i) + 0.5) / 5.0;
    return pow(texture(scatterDistLUT, vec2(u, 0.5)).rgb, vec3(2.2));
}

// Thickness-based blend across the 5 scatter distance profiles
vec3 scatterDistanceBlend(float thickness) {
    float t      = clamp(thickness, 0.0, 1.0);
    float scaled = clamp(t * 4.0, 0.0, 4.0);
    int   i0     = int(floor(scaled));
    int   i1     = min(i0 + 1, 4);
    float w      = scaled - float(i0);
    return mix(sampleProfile(i0), sampleProfile(i1), w);
}

// High-quality Interleaved Gradient Noise
float interleavedGradientNoise(vec2 pix) {
    return fract(52.9829189 * fract(dot(pix, vec2(0.06711056, 0.00583715))));
}

vec3 Rr(vec3 d, float r) // Burley's Normalized Diffusion Model
{ 
  return (exp(-r / d) + exp(-r / (d * 3.0))) / (8.0 * PI * d * r);
}

// ----------------------------------------------------------------------------
void main() {
    vec4  albedoMask  = texture(albedoMaskTex, v_uv);
    float scatterMask = albedoMask.a;

    // Non-skin pixels (hair, unlit, sky) — pass through unchanged
    if (scatterMask == 0.0) {
        outColor  = texture(hdrTex, v_uv);
        outBright = texture(brightTex, v_uv);
        return;
    }

    vec3  albedo  = albedoMask.rgb;
    vec4  hdr     = texture(hdrTex, v_uv);
    vec3  diffIrr = texture(diffuseIrrTex, v_uv).rgb;
    vec3  backIrr = texture(backIrrTex, v_uv).rgb;
    float depth   = texture(depthTex, v_uv).r;
    vec2  aoThick = texture(aoThickTex, v_uv).rg;
    float ao      = aoThick.r;

    // 3x3 box blur on thickness only. The G channel from SSAO is low-frequency
    // and noticeably quantized — blurring it removes the chunked stepping in
    // the spectral transmittance without affecting the physical formulation.
    vec2  texelSize = 1.0 / sss.screenSize;
    float thick = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            thick += texture(aoThickTex, v_uv + vec2(dx, dy) * texelSize).g;
        }
    }
    thick /= 9.0;

	if (sss.sampleCount == 0) {
        outColor  = texture(hdrTex, v_uv) * ao;
        outBright = texture(brightTex, v_uv);
        return;
    }

    if (depth >= 1.0) {   // background — should not happen for masked pixels, be safe
        outColor  = hdr;
        outBright = texture(brightTex, v_uv);
        return;
    }

    vec3 blendedScatterDist = scatterDistanceBlend(thick);
    vec3 scatterDist = blendedScatterDist * sss.maxScatter;
	float maxRadius = max(scatterDist.r, max(scatterDist.g, scatterDist.b));

    // -------------------------------------------------------------------------
    // Multiple scattering
    //
    // The forward pass HDR buffer contains:
    //   hdr = albedo * diffIrr + specular + ambient
    //
    // We replace the local diffuse term with the scattered diffuse integral
    // and keep everything else (specular, ambient) unchanged.
    // -------------------------------------------------------------------------

	// View fragment position computation (perspectiveRH_ZO: depth already in [0,1])
  	vec2 fragCoords = v_uv * 2.0 - vec2(1.0);
  	vec4 viewSpacePos = sss.invProjection * vec4(fragCoords, depth, 1.0);
  	vec3 fragViewPos = viewSpacePos.xyz / viewSpacePos.w;

    float jitter = 2.0 * PI * interleavedGradientNoise(gl_FragCoord.xy);

    vec3 scatteredIrr = vec3(0.0);
    vec3 totalWeight  = vec3(0.0);

    for (int i = 0; i < sss.sampleCount; i++) {
		float theta = sss.samples[i].x + jitter;
        float r     = sss.samples[i].y * maxRadius;
        vec2  sampleOffset  = vec2(cos(theta), sin(theta)) * r;

        // Offset the view-space fragment position in the XY plane (lateral scatter)
        vec4 sampleProjected = sss.projection * vec4(fragViewPos + vec3(sampleOffset, 0.0), 1.0);
        vec2 sampleCoords = sampleProjected.xy / sampleProjected.w;
        vec2 sampleUV = (sampleCoords + 1.0) * 0.5;

		float sampleDepth = texture(depthTex, sampleUV).r;
    	viewSpacePos = sss.invProjection * vec4(sampleCoords, sampleDepth, 1.0);
    	vec3 sampleViewPos = viewSpacePos.xyz / viewSpacePos.w;

    	float radialDistance = max(distance(sampleViewPos, fragViewPos), EPS);

        // Per-channel Burley weight — normalized below to handle spectral bias
        vec3 rRr = radialDistance * Rr(scatterDist, radialDistance);
		vec3 pr = r * Rr(vec3(maxRadius), r);
		vec3 diffusion = rRr / pr;
		totalWeight += diffusion;

		vec3  sampleDiffIrr = texture(diffuseIrrTex, sampleUV).rgb;
        scatteredIrr       += diffusion * sampleDiffIrr;
    }

    // Normalize per channel
	scatteredIrr = (albedo / PI) * (scatteredIrr / max(totalWeight, vec3(EPS)));

    // -------------------------------------------------------------------------
    // Single scattering (translucency)
    //
    //   S_s = F_t(ωi) · F_t(ωo) · p(cosθ, g) · exp(-σ_t · s) · L_back
    //
    // (A) Per-channel σ_t derived from the scatter-distance LUT (1/d) — gives
    //     the characteristic spectral falloff (red transmits deeper than green/blue).
    // (C) Henyey-Greenstein phase with g = 0.8 (forward-scattering skin).
    //     Cheap approximation: cosθ ≈ N·V (light assumed roughly opposite view
    //     through the surface, hitting the back face along its normal).
    // (D) Fresnel entry × exit at η = 1.4 via Schlick. Supersedes the artist
    //     `sss.Fdr` uniform, which is now unused (directional Fresnel covers it).
    // -------------------------------------------------------------------------

    // Reconstruct view-space normal from depth. Sample 4 neighbours and pick
    // the side with the smaller view-space-z jump along each axis: this keeps
    // the cross product on the same surface at silhouettes / depth jumps,
    // killing the chunky banding the naive dFdx/dFdy version produced.
    vec4 nL = sss.invProjection * vec4((v_uv - vec2(texelSize.x, 0.0)) * 2.0 - 1.0,
                                       texture(depthTex, v_uv - vec2(texelSize.x, 0.0)).r, 1.0);
    vec4 nR = sss.invProjection * vec4((v_uv + vec2(texelSize.x, 0.0)) * 2.0 - 1.0,
                                       texture(depthTex, v_uv + vec2(texelSize.x, 0.0)).r, 1.0);
    vec4 nU = sss.invProjection * vec4((v_uv + vec2(0.0, texelSize.y)) * 2.0 - 1.0,
                                       texture(depthTex, v_uv + vec2(0.0, texelSize.y)).r, 1.0);
    vec4 nD = sss.invProjection * vec4((v_uv - vec2(0.0, texelSize.y)) * 2.0 - 1.0,
                                       texture(depthTex, v_uv - vec2(0.0, texelSize.y)).r, 1.0);
    vec3 vpL = nL.xyz / nL.w;
    vec3 vpR = nR.xyz / nR.w;
    vec3 vpU = nU.xyz / nU.w;
    vec3 vpD = nD.xyz / nD.w;

    vec3 hDeriv = abs(vpR.z - fragViewPos.z) < abs(fragViewPos.z - vpL.z)
                  ? (vpR - fragViewPos) : (fragViewPos - vpL);
    vec3 vDeriv = abs(vpU.z - fragViewPos.z) < abs(fragViewPos.z - vpD.z)
                  ? (vpU - fragViewPos) : (fragViewPos - vpD);

    vec3 viewNormal = normalize(cross(hDeriv, vDeriv));
    if (viewNormal.z < 0.0) viewNormal = -viewNormal;

    vec3  viewDir = normalize(-fragViewPos);
    float NoV     = max(dot(viewNormal, viewDir), 0.0);

    // Henyey-Greenstein phase
    const float g       = 0.8;
    float       hgDenom = 1.0 + g * g - 2.0 * g * NoV;
    float       phase   = (1.0 - g * g) / (4.0 * PI * pow(max(hgDenom, EPS), 1.5));

    // Fresnel transmission at entry and exit (symmetric under the N·V approx)
    const float eta = 1.4;
    const float F0  = ((1.0 - eta) * (1.0 - eta)) / ((1.0 + eta) * (1.0 + eta));
    float       Fc  = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);
    float       Ft  = 1.0 - Fc;
    float       fresnelTransmission = Ft * Ft;

    // Per-channel extinction. `extinctionCoeff` now acts as a thickness-scale
    // (converts the 0–1 `thick` proxy into path-length units); spectral shape
    // comes from σ_t = 1 / d_rgb.
    vec3 sigmaT       = 1.0 / max(blendedScatterDist, vec3(EPS));
    vec3 transmittance = exp(-thick * sss.extinctionCoeff * sigmaT);

    vec3 singleScatter = fresnelTransmission * phase * transmittance * backIrr;

    // -------------------------------------------------------------------------
    // Combine and output
    // -------------------------------------------------------------------------
    vec3 specular = max(hdr.rgb - scatteredIrr, vec3(0.0));
    outColor  = vec4((scatteredIrr + singleScatter + specular) * ao, hdr.a);
    outBright = texture(brightTex, v_uv);
}
