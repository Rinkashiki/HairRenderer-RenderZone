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

// =============================================================================
// Screen-space subsurface scattering — leaf version.
//
// Multiple scattering only: the diffuse irradiance written by the forward pass is
// blurred with Burley's normalized diffusion profile, using one global per-channel
// scatter distance. The reflected (diffIrr) and transmitted (backIrr) Lambert
// irradiances are blurred separately, the transmitted one with the kernel radius
// scaled by SCATTER_FACTOR: light that crossed the blade has travelled further
// inside it. Light through the blade itself comes from the thin-surface BSDF's
// transmission term, so there is no single-scattering term, and thickness is
// assumed constant (no thickness LUT).
// The skin version (thickness LUT + single scattering) is kept in ssss_humans.glsl.
// =============================================================================

layout(location = 0) in vec2 v_uv;

// --- Input textures (all from ForwardPass MRTs + SSAO pass) -----------------
// Binding 8 (scatter LUT) still exists in SSSPass's layout (used by
// ssss_humans.glsl) but is not read here.
layout(set = 0, binding = 0) uniform sampler2D hdrTex;        // full HDR shading
layout(set = 0, binding = 1) uniform sampler2D albedoMaskTex; // RGB = albedo, A = scatterMask
layout(set = 0, binding = 2) uniform sampler2D diffuseIrrTex; // reflected Lambert irradiance (kD baked in, no albedo)
layout(set = 0, binding = 3) uniform sampler2D backIrrTex;    // transmitted Lambert irradiance (same convention)
layout(set = 0, binding = 4) uniform sampler2D depthTex;      // R = gl_FragCoord.z
layout(set = 0, binding = 5) uniform sampler2D aoThickTex;    // R = AO (G = thickness, unused)
layout(set = 0, binding = 7) uniform sampler2D brightTex;     // Bright pass-through for Bloom

// --- Uniform block (std140, binding 6) — layout shared with ssss_humans.glsl -
layout(set = 0, binding = 6) uniform SSSBlock {
    // xy = (theta, r), zw = unused (vec4 for std140 array padding).
    // r is importance-sampled from the Burley profile for d = 1.
    vec4  samples[64];

    int   sampleCount;
    float _unused0;         // maxScatter      (skin version only)
    float _unused1;         // extinctionCoeff (skin version only)
    float _unused2;         // Fdr             (skin version only)

    vec2  screenSize;

    layout(offset = 1056) mat4 projection;
    mat4 invProjection;
    vec4 scatterDistance;   // xyz = per-channel Burley scatter distance d, view-space (= world) units
} sss;

// --- Outputs ----------------------------------------------------------------
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outBright; // pass-through to Bloom

// ----------------------------------------------------------------------------
const float PI  = 3.14159265358979323846;
const float EPS = 1e-6;

// High-quality Interleaved Gradient Noise
float interleavedGradientNoise(vec2 pix) {
    return fract(52.9829189 * fract(dot(pix, vec2(0.06711056, 0.00583715))));
}

// Burley's normalized diffusion profile R(r), per channel, for scatter distance d
vec3 Rr(vec3 d, float r) {
    return (exp(-r / d) + exp(-r / (d * 3.0))) / (8.0 * PI * d * r);
}

// > 0. Scales the blur kernel radius (scatter distance) for the transmitted
// irradiance relative to the reflected one.
const float SCATTER_FACTOR = 2.0;

// Blurs an irradiance buffer around the fragment with Burley's profile for the
// per-channel scatter distance d. Returns the per-channel normalised result
// (without albedo / PI).
vec3 blurIrradiance(sampler2D irrTex, vec3 d, vec3 fragViewPos, float jitter) {
    // Taps are importance-sampled for the widest channel; the others are reweighted
    float maxRadius = max(d.r, max(d.g, d.b));

    vec3 scatteredIrr = vec3(0.0);
    vec3 totalWeight  = vec3(0.0);

    for (int i = 0; i < sss.sampleCount; i++) {
        float theta        = sss.samples[i].x + jitter;
        float r            = sss.samples[i].y * maxRadius;
        vec2  sampleOffset = vec2(cos(theta), sin(theta)) * r;

        // Offset the view-space fragment position in the XY plane (lateral scatter)
        vec4 sampleProjected = sss.projection * vec4(fragViewPos + vec3(sampleOffset, 0.0), 1.0);
        vec2 sampleCoords    = sampleProjected.xy / sampleProjected.w;
        vec2 sampleUV        = (sampleCoords + 1.0) * 0.5;

        float sampleDepth  = texture(depthTex, sampleUV).r;
        vec4  viewSpacePos = sss.invProjection * vec4(sampleCoords, sampleDepth, 1.0);
        vec3  sampleViewPos = viewSpacePos.xyz / viewSpacePos.w;

        // The tap was drawn for distance r on the view plane (pdf of the widest
        // channel); the surface point it landed on is at r'. Reweight each channel
        // by its profile(r') / pdf(r), so taps that hit geometry far in depth
        // contribute little and narrower channels fall off faster.
        float radialDistance = max(distance(sampleViewPos, fragViewPos), EPS);
        vec3  rRr            = radialDistance * Rr(d, radialDistance);
        vec3  pr             = r * Rr(vec3(maxRadius), r);
        vec3  diffusion      = rRr / max(pr, vec3(EPS));
        totalWeight  += diffusion;
        scatteredIrr += diffusion * texture(irrTex, sampleUV).rgb;
    }

    // Normalize per channel
    return scatteredIrr / max(totalWeight, vec3(EPS));
}

// ----------------------------------------------------------------------------
void main() {
    vec4  albedoMask  = texture(albedoMaskTex, v_uv);
    float scatterMask = albedoMask.a;

    // Pixels that don't scatter — pass through unchanged
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
    float ao      = texture(aoThickTex, v_uv).r;

    // SSS disabled — keep the AO the enabled path applies
    if (sss.sampleCount == 0) {
        outColor  = hdr * ao;
        outBright = texture(brightTex, v_uv);
        return;
    }

    if (depth >= 1.0) {   // background — should not happen for masked pixels, be safe
        outColor  = hdr;
        outBright = texture(brightTex, v_uv);
        return;
    }

    vec3 d = max(sss.scatterDistance.xyz, vec3(EPS));

    // -------------------------------------------------------------------------
    // Multiple scattering
    //
    // The forward pass HDR buffer contains:
    //   hdr = (albedo / PI) * (diffIrr + backIrr) + everything else (specular, ...)
    //
    // We replace the two local diffuse terms with their scattered integrals —
    // the transmitted one blurred with a SCATTER_FACTOR times wider kernel — and
    // keep everything else unchanged.
    // -------------------------------------------------------------------------

    // View fragment position computation (perspectiveRH_ZO: depth already in [0,1])
    vec2 fragCoords   = v_uv * 2.0 - vec2(1.0);
    vec4 viewSpacePos = sss.invProjection * vec4(fragCoords, depth, 1.0);
    vec3 fragViewPos  = viewSpacePos.xyz / viewSpacePos.w;

    float jitter = 2.0 * PI * interleavedGradientNoise(gl_FragCoord.xy);

    vec3 scatteredDiff = blurIrradiance(diffuseIrrTex, d,                  fragViewPos, jitter);
    vec3 scatteredBack = blurIrradiance(backIrrTex,    d * SCATTER_FACTOR, fragViewPos, jitter);

    // -------------------------------------------------------------------------
    // Combine and output
    //
    // `localDiff` reconstructs exactly the diffuse summed into `hdr` (the forward
    // pass writes diffIrr and backIrr to match), so `hdr - localDiff` is the exact
    // non-diffuse remainder. scatterMask fades between the local and the
    // scattered diffuse.
    // -------------------------------------------------------------------------
    vec3 localDiff     = (albedo / PI) * (diffIrr + backIrr);
    vec3 scatteredIrr  = (albedo / PI) * (scatteredDiff + scatteredBack);
    vec3 modulatedDiff = mix(localDiff, scatteredIrr, scatterMask);
    vec3 remainder     = hdr.rgb - localDiff;
    outColor  = vec4((modulatedDiff + remainder) * ao, hdr.a);
    outBright = texture(brightTex, v_uv);
}
