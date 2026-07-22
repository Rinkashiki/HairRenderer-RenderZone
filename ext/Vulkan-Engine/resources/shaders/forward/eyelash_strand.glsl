#shader vertex
#version 460 core
#include object.glsl

// Input
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec3 tangent;
layout(location = 4) in vec3 color;

// Output
layout(location = 0) out vec3 v_color;
layout(location = 1) out vec3 v_tangent;
layout(location = 2) out vec2 v_uv;

void main() {

    gl_Position = object.model * vec4(position, 1.0);

    v_tangent = normalize(mat3(transpose(inverse(object.model))) * tangent);
    v_color   = color;
    v_uv      = uv;
}

#shader geometry
#version 460 core
#include camera.glsl

// Setup
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

// Input
layout(location = 0) in vec3 v_color[];
layout(location = 1) in vec3 v_tangent[];
layout(location = 2) in vec2 v_uv[];

// Uniforms. Declared out to slot 9 (not just the first two fields like the epic
// hair geometry shader) because the taper and sub-pixel coverage models need the
// eyelash block here, and std140 offsets only line up if everything before it is
// declared. `_slotN` members are the epic-hair params this stage doesn't use.
layout(set = 1, binding = 1) uniform MaterialUniforms {
    vec3  baseColor;
    float thickness;

    vec4 _slot2;
    vec4 _slot3;
    vec4 _slot4;
    vec4 _slot5;
    vec4 _slot6;
    vec4 _slot7;
    vec4 _slot8;

    float variant;
    float sheenScale;
    float tipTaper;
    float minPixelWidth;
}
material;

#define VARIANT_BASELINE 0
#define VARIANT_MATTE    1
#define VARIANT_TAPERED  2
#define VARIANT_COVERAGE 3

// Output
layout(location = 0) out vec3 g_pos;
layout(location = 1) out vec3 g_modelPos;
layout(location = 2) out vec3 g_normal;
layout(location = 3) out vec3 g_modelNormal;
layout(location = 4) out vec2 g_uv;
layout(location = 5) out vec3 g_dir;
layout(location = 6) out vec3 g_modelDir;
layout(location = 7) out vec3 g_color;
layout(location = 8) out vec3 g_origin;
layout(location = 9) out float g_coverage;

// Fraction of the pixel the fiber actually covers; 1.0 unless the sub-pixel
// coverage model widened it. Set once per strand, read by emitQuadPoint.
float coverage = 1.0;

void emitQuadPoint(vec4 origin, vec4 right, float offset, vec3 forward, vec3 normal, vec2 uv, int id) {

    g_coverage    = coverage;
    vec4 newPos   = origin + right * offset; // Model space
    gl_Position   = camera.viewProj * newPos;
    g_dir         = normalize(mat3(transpose(inverse(camera.view))) * v_tangent[id]);
    g_modelDir    = v_tangent[id];
    g_color       = v_color[id];
    g_pos         = (camera.view * newPos).xyz;
    g_modelPos    = newPos.xyz;
    g_uv          = uv;
    g_normal      = normalize(mat3(transpose(inverse(camera.view))) * normal);
    g_modelNormal = normal;
    g_origin      = (camera.view * origin).xyz;

    EmitVertex();
}

void main() {

    const vec2 VIEWPORT_SIZE = vec2(camera.screenExtent);

    // Model space --->>>

    vec4 startPoint = gl_in[0].gl_Position;
    vec4 endPoint   = gl_in[1].gl_Position;

    vec4 view0 = vec4(camera.position.xyz, 1.0) - startPoint;
    vec4 view1 = vec4(camera.position.xyz, 1.0) - endPoint;

    vec3 dir0 = v_tangent[0];
    vec3 dir1 = v_tangent[1];

    vec4 right0 = normalize(vec4(cross(dir0.xyz, view0.xyz), 0.0));
    vec4 right1 = normalize(vec4(cross(dir1.xyz, view1.xyz), 0.0));

    vec3 normal0 = normalize(cross(right0.xyz, dir0.xyz));
    vec3 normal1 = normalize(cross(right1.xyz, dir1.xyz));

    //<<<----

    int variant = int(material.variant);

    float half0 = material.thickness * 0.5;
    float half1 = half0;

    // TAPERED: real lashes are thickest at the lid and thin toward the tip. uv.x
    // runs root(0)->tip(1) along the strand (same param applyNaturalVariation
    // uses), so each endpoint gets its own width and the fiber narrows smoothly.
    if (variant == VARIANT_TAPERED)
    {
        const float TIP_WIDTH = 0.3; // tip width at full taper, as a fraction of the root
        half0 *= mix(1.0, mix(1.0, TIP_WIDTH, v_uv[0].x), material.tipTaper);
        half1 *= mix(1.0, mix(1.0, TIP_WIDTH, v_uv[1].x), material.tipTaper);
    }

    // COVERAGE: a fiber thinner than a pixel cannot be rasterized at its true
    // width — it either drops out or, as here, gets drawn a full pixel wide at
    // full opacity, which is what reads as a hard shiny wire. Instead widen it to
    // the pixel floor and hand the fragment shader the fraction of the pixel it
    // really covers, so the energy stays put (alpha-to-coverage does the rest).
    if (variant == VARIANT_COVERAGE)
    {
        vec4 c0 = camera.viewProj * startPoint;
        vec4 c1 = camera.viewProj * (startPoint + right0 * half0);

        // Behind the near plane the perspective divide is meaningless — leave the
        // strand at its physical width rather than computing a garbage scale.
        if (c0.w > 1e-4 && c1.w > 1e-4)
        {
            vec2  s0      = (c0.xy / c0.w) * VIEWPORT_SIZE * 0.5;
            vec2  s1      = (c1.xy / c1.w) * VIEWPORT_SIZE * 0.5;
            float widthPx = 2.0 * length(s1 - s0);

            if (widthPx < material.minPixelWidth && widthPx > 1e-4)
            {
                float scaleFactor = material.minPixelWidth / widthPx;
                half0 *= scaleFactor;
                half1 *= scaleFactor;
                coverage = 1.0 / scaleFactor;
            }
        }
    }

    emitQuadPoint(startPoint, right0, half0, dir0, normal0, v_uv[0], 0);
    emitQuadPoint(endPoint, right1, half1, dir1, normal1, v_uv[1], 1);
    emitQuadPoint(startPoint, -right0, half0, dir0, normal0, v_uv[0], 0);
    emitQuadPoint(endPoint, -right1, half1, dir1, normal1, v_uv[1], 1);
}

#shader fragment
#version 460 core
#include light.glsl
#include scene.glsl
#include camera.glsl
#include object.glsl
#include utils.glsl
#include shadow_mapping.glsl
#include reindhart.glsl
#include sh.glsl
#include BRDFs/epic_hair_BSDF.glsl

// Input
layout(location = 0) in vec3 g_pos;
layout(location = 1) in vec3 g_modelPos;
layout(location = 2) in vec3 g_normal;
layout(location = 3) in vec3 g_modelNormal;
layout(location = 4) in vec2 g_uv;
layout(location = 5) in vec3 g_dir;
layout(location = 6) in vec3 g_modelDir;
layout(location = 7) in vec3 g_color;
layout(location = 8) in vec3 g_origin;
layout(location = 9) in float g_coverage;

// Uniforms
layout(set = 0, binding = 2) uniform sampler2DArray shadowMap;
layout(set = 0, binding = 4) uniform samplerCube irradianceMap;

layout(set = 0, binding = 7) uniform sampler3D NpTex;
layout(set = 0, binding = 9) uniform sampler3D hairVoxelsSh;
layout(set = 0, binding = 13) uniform sampler3D hairVoxelsDensity;
layout(set = 0, binding = 12) uniform sampler3D hairLUT;

layout(push_constant) uniform Data {
    float id;
    float avgFiberLength;
}
data;

layout(set = 1, binding = 1) uniform MaterialUniforms {
    vec3  baseColor;
    float thickness;

    float roughness;
    float metallic;
    float specular;
    float shift;

    float ior;
    float Rpower;
    float TTpower;
    float TRTpower;

    float opaqueVisibility;
    float useLegacyAbsorption;
    float useSeparableR;
    float useBacklit;

    float clampBSDFValue;
    float r;
    float tt;
    float trt;

    float scatter;
    float scatterKnob;
    float advShadows;
    float shadowKnob;

    float useGlints;
    float rootDarkening;
    float tipBleaching;
    float tipFalloff;

    float variability;
    // Declared as three floats, NOT vec3: under std140 a vec3 here would be
    // padded up to offset 128 (16-byte alignment) while the CPU packs the tint
    // at 116, so everything declared after it would land one slot late.
    float tintR;
    float tintG;
    float tintB;

    // Eyelash-only block (slot 9). Epic hair leaves slots 9-11 unused.
    float variant;       // EyelashMaterial::Variant — which lighting model to run
    float sheenScale;    // env specular sheen damp
    float tipTaper;      // root->tip thickness / transmission taper
    float minPixelWidth; // sub-pixel coverage floor
}
material;

// Lighting models under evaluation. One pipeline, selected per material, so all
// of them can be compared in a single frame under identical lighting.
#define VARIANT_BASELINE 0
#define VARIANT_MATTE    1
#define VARIANT_TAPERED  2
#define VARIANT_COVERAGE 3

EpicHairBSDF bsdf;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 outBrightColor;
layout(location = 2) out vec4 outNormals;
layout(location = 3) out vec4 outAlbedoMask;
layout(location = 4) out vec4 outDiffuseIrr;
layout(location = 5) out vec4 outBackIrr;
layout(location = 6) out vec4 outLinearDepth;

// Hair ambient. Two failure modes the previous flat-ambient version had:
//  - Dark hair (high eumelanin) brightened just as much as blonde from ambient,
//    because the term ignored absorption.
//  - The `if (scene.useIBL)` branch existed but only computed `rotatedNormal`
//    and dropped it on the floor — `ambient` was never set, so cubemap energy
//    never reached hair fibers.
// Both are fixed by sampling the irradiance cubemap along the "head-outward"
// fake normal and modulating by the hair base color (which already encodes
// melanin-driven absorption via getAbsorptionFromMelanin → hairAbsorptionToColor).
vec3 computeAmbient(vec3 n, vec3 hairColor) {
    if (scene.useIBL)
    {
        float rad           = radians(scene.envRotation);
        float c             = cos(rad);
        float s             = sin(rad);
        mat3  rotationY     = mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
        vec3  rotatedNormal = normalize(rotationY * n);
        vec3  irradiance    = texture(irradianceMap, rotatedNormal).rgb * scene.ambientIntensity;
        return irradiance * hairColor;
    }
    return scene.ambientIntensity * scene.ambientColor * hairColor;
}

// Anysotropic. Decoding from a L1 SH
float getOpticalDensity(vec3 worldPos, vec3 lightWorldPos) {
    vec3 dir = normalize(lightWorldPos - worldPos);

    // Compute voxel UVW coords in object space
    vec3 uvw = (worldPos - object.minCoord.xyz) / (object.maxCoord.xyz - object.minCoord.xyz);
    uvw      = clamp(uvw, 0.0, 0.9999);

    // Fetch SH L1 and decode
    // ivec3 coord = ivec3(uvw * vec3(textureSize(hairVoxelsSh, 0)));
    // vec4 SHL1 = texelFetch(hairVoxelsSh, coord, 0);
    vec4 SHL1 = texture(hairVoxelsSh, uvw, 0);

    return decodeScalarFromSHL1(SHL1, dir);
}

// Construye una base ortonormal basada en la Tangente y la Vista
// T = Tangente del pelo (g_dir)
// V = Vector Vista (hacia la cámara)
void buildBasis(vec3 T, vec3 V, out vec3 N, out vec3 B) {
    // 1. Binormal (B): Es el vector que va "a lo ancho" de la cinta en pantalla.
    // Es perpendicular a la hebra (T) y a la mirada (V).
    // Degenerate case: T parallel to V → cross is zero → normalize is NaN. Pick
    // any arbitrary perpendicular axis instead so the BSDF stays finite.
    vec3 raw = cross(T, V);
    float rawLen2 = dot(raw, raw);
    if (rawLen2 < 1e-8) {
        vec3 fallback = abs(T.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        raw = cross(T, fallback);
    }
    B = normalize(raw);

    // 2. Normal (N): Es el vector que apunta "hacia fuera" del cilindro.
    // Es perpendicular a la Binormal y a la Tangente. B ⟂ T by construction,
    // so cross(B, T) is non-zero and normalize is safe.
    N = normalize(cross(B, T));
}
float hash31(vec3 p3) {
    p3 = fract(p3 * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec3 computeMicroTangent(vec3 T, vec3 V, vec3 worldPos) {

    // --- 1. RECONSTRUIR BASE VIRTUAL ---
    vec3 N, B;
    buildBasis(T, V, N, B);

    // --- 2. CONFIGURACIÓN ---
    // float glintStrength = material.TTpower;    // Fuerza (3)
    // // float glintScale    = 50.0; // Frecuencia (Escamas muy pequeñas)
    // float glintScale    = material.TRTpower; // Frecuencia 150 (Escamas muy pequeñas)
    // float anisotropy    = material.Rpower;   // Estiramiento (0.01 = muy estirado a lo largo)
    float glintStrength = 0.2;  // Fuerza (3)
    float glintScale    = 50.0; // Frecuencia 150 (Escamas muy pequeñas)
    float anisotropy    = 0.01; // Estiramiento (0.01 = muy estirado a lo largo)

    // // --- 3. LOD (Anti-Aliasing) ---
    // // Derivada de posición: ¿Cuánto terreno cubrimos por pixel?
    // float pixelSize = length(fwidth(worldPos));
    // // Si el pixel es más grande que el ruido, apagamos el efecto suavemente
    // float lodFade = 1.0 - smoothstep(0.0, 1.0, pixelSize * glintScale * 0.5);

    // if (lodFade < 0.01) return T;

    // --- 4. COORDENADAS DE RUIDO ---
    // Proyectamos la posición del mundo sobre nuestro cilindro virtual
    float u = dot(worldPos, T); // Largo de la hebra
    float v = dot(worldPos, B); // Ancho de la hebra

    // Estiramos para crear anisotropía (escamas)
    vec3 noiseCoord = vec3(u * anisotropy, v, 0.0) * glintScale;

    // --- 5. RUIDO ---
    float n = hash31(floor(noiseCoord));

    // Mapear [0,1] -> [-1, 1]
    float perturbation = (n * 2.0 - 1.0);

    // --- 6. APLICAR ---
    // Movemos la tangente hacia la Binormal (ancho) y un poco hacia la Normal (profundidad)
    // para dar sensación 3D.
    vec3 T_micro = normalize(T + (B * perturbation) * glintStrength);

    return T_micro;
}

float computeHairShadowCone(vec3 worldPos, vec3 lightDir, vec3 physicalAbsorption, float densityScale) {

    vec3  boundsMin = object.minCoord.xyz;
    vec3  boundsMax = object.maxCoord.xyz;
    vec3  boxSize   = boundsMax - boundsMin;
    float maxBoxDim = max(boxSize.x, max(boxSize.y, boxSize.z));

    ivec3 texDim            = textureSize(hairVoxelsDensity, 0);
    float maxTexDim         = float(max(texDim.x, max(texDim.y, texDim.z)));
    float voxelSizeTexSpace = 1.0 / maxTexDim;

    vec3 startTexPos = (worldPos - boundsMin) / boxSize;

    // Vinculacion con la fisica del pelo
    float materialSigma = max(physicalAbsorption.r, max(physicalAbsorption.g, physicalAbsorption.b));

    // Factor de corrección empírico (porque voxel density != path distance pura)
    // Este número ya no variará tanto entre rubio/moreno, será más estable.
    float dynamicSigma      = materialSigma ;

    float coneAngle    = 0.035;
    float minStepWorld = length(boxSize) * voxelSizeTexSpace * 0.5;

    // 3. INICIO RAYO
    float t     = minStepWorld * 2.0; // No autosombra
    float trans = 1.0;
    float tMax  = 1.75;

    // Nunca avanzar más de un 5% del volumen en un solo paso, aunque el cono sea enorme.
    float maxStepWorld = maxBoxDim * 0.1;

    const int MAX_STEPS = 64;
    for (int i = 0; i < MAX_STEPS &&  trans > 0.01; i++)
    {
        // 1. Calcular posición en textura [0..1]
        vec3 samplePos = startTexPos + ((lightDir * t) / boxSize);

        // 2. CHECK DE SALIDA: Si salimos del volumen, no hay más pelo que ocluya.
        if (any(lessThan(samplePos, vec3(0.0))) || any(greaterThan(samplePos, vec3(1.0))))
        {
            break; 
        }

        // 3. RADIOS SEPARADOS (Mundo vs Textura)
        float coneRadiusWorld = t * tan(coneAngle);            // Radio físico
        float coneRadiusTex   = coneRadiusWorld / maxBoxDim;   // Radio normalizado para el MIP

        // 4. MIP LEVEL
        float mipLevel = log2(max(coneRadiusTex, 1e-6) / voxelSizeTexSpace);
        mipLevel = clamp(mipLevel, 0.0, 3.0);

        // 5. SAMPLEO
        float density = textureLod(hairVoxelsDensity, samplePos, mipLevel).r;

        // 6. TAMAÑO DEL PASO EN UNIDADES DE MUNDO
        // Avanzamos el diámetro del cono (Radius * 2), pero limitándolo por los min/max.
        float stepSize = clamp(coneRadiusWorld * 2.0, minStepWorld, maxStepWorld);

        // 7. BEER-LAMBERT
        if (density > 0.001)
        {
            trans *= exp(-density * dynamicSigma * stepSize * densityScale);
        }

        // 8. AVANZAR
        t += stepSize;
    }

    return trans;
}

float bilinear(float v[4], vec2 f) {
    return mix(mix(v[0], v[1], f.x), mix(v[2], v[3], f.x), f.y);
}

vec3 bilinear(vec3 v[4], vec2 f) {
    return mix(mix(v[0], v[1], f.x), mix(v[2], v[3], f.x), f.y);
}
// Compute hair-fiber transmittance along the ray from `worldPos` toward the
// light by cone-tracing the shared hairVoxelsDensity volume. Replaces a prior
// scheme that treated the VSM depth gap (receiver_depth − closest_caster_depth)
// as a packed-fiber column — that was fine for a single hair asset but, with
// two hair assets, interpreted the air gap between them as a wall of fibers
// and projected a sharp silhouette of the second asset onto the first.
//
// The voxel volume is now coherent across multiple hair meshes: ResourceManager
// substitutes the union world AABB on every strand-hair ObjectUniforms upload,
// and HairVoxelizationPass dispatches the voxelization compute for every active
// strand mesh into the same volume via imageAtomicAdd.
vec3 hairShadow(out vec3 spread, out float directF, vec3 worldPos, vec3 lightWorldPos, float densityScale) {
    const float coverage = 0.05;
    const vec3  a_f      = vec3(0.507475266, 0.465571405, 0.394347166);
    const vec3  w_f      = vec3(0.028135575, 0.027669785, 0.027669785);

    vec3  boundsMin = object.minCoord.xyz;
    vec3  boundsMax = object.maxCoord.xyz;
    vec3  boxSize   = boundsMax - boundsMin;
    float maxBoxDim = max(boxSize.x, max(boxSize.y, boxSize.z));

    ivec3 texDim            = textureSize(hairVoxelsDensity, 0);
    float maxTexDim         = float(max(texDim.x, max(texDim.y, texDim.z)));
    float voxelSizeTexSpace = 1.0 / maxTexDim;

    vec3  lightDir     = normalize(lightWorldPos - worldPos);
    vec3  startTexPos  = (worldPos - boundsMin) / boxSize;
    float coneAngle    = 0.035;
    float minStepWorld = length(boxSize) * voxelSizeTexSpace * 0.5;
    float maxStepWorld = maxBoxDim * 0.1;

    // Accumulate fiber count n along the cone. Empirical scale chosen so single-
    // asset behavior roughly matches the old `h * density * 10000` magnitude:
    // the OLD formula was h(NDC) * density(0.7) * 10000 ≈ 7000·h, and a typical
    // h in self-shadow was ~0.001 giving n ≈ 7. Here per-step contribution is
    // density_voxel · stepSize · densityScale · FIBER_SCALE; FIBER_SCALE is the
    // single knob that absorbs voxel-unit conventions.
    const float FIBER_SCALE = 10.0;

    float n    = 0.0;
    float t    = minStepWorld * 2.0; // step off the receiver so we don't self-shadow at t=0
    float tMax = 1.75;
    const int MAX_STEPS = 64;
    for (int i = 0; i < MAX_STEPS && t < tMax; i++) {
        vec3 samplePos = startTexPos + ((lightDir * t) / boxSize);
        if (any(lessThan(samplePos, vec3(0.0))) || any(greaterThan(samplePos, vec3(1.0))))
            break;

        float coneRadiusWorld = t * tan(coneAngle);
        float coneRadiusTex   = coneRadiusWorld / maxBoxDim;
        float mipLevel        = clamp(log2(max(coneRadiusTex, 1e-6) / voxelSizeTexSpace), 0.0, 3.0);
        float density         = textureLod(hairVoxelsDensity, samplePos, mipLevel).r;
        float stepSize        = clamp(coneRadiusWorld * 2.0, minStepWorld, maxStepWorld);

        n += density * stepSize * densityScale * FIBER_SCALE;
        t += stepSize;
    }

    directF = pow(1.0 - coverage, n);
    spread  = n * coverage * w_f;
    return pow(1.0 - coverage * (1.0 - a_f), vec3(n, n, n));
}

vec3 computeHairShadow(LightUniform light, int lightId, sampler2DArray shadowMap, float density, vec3 pos, out vec3 spread, out float directF) {
    // Light positions in the scene buffer are stored in view space; transform
    // to world so the cone trace lines up with worldPos = g_modelPos.
    vec3 lightWorldPos = (camera.invView * vec4(light.position, 1.0)).xyz;
    vec3 transDirect   = hairShadow(spread, directF, pos, lightWorldPos, density);
    directF *= 0.5;
    return transDirect * 0.5;
}

void applyNaturalVariation(inout float m, inout float r, float uv_length, float variation) {

    float bleachFactor = pow(uv_length, material.tipFalloff);
    float darkenactor = pow(1.0 - uv_length, material.tipFalloff);

    m += material.rootDarkening *darkenactor; 
    m -= material.tipBleaching * bleachFactor;       

    float strandVariationStrength = material.variability; 

    float randomOffset = (variation * 2.0 - 1.0) * strandVariationStrength;
    m += randomOffset;
    r += randomOffset;

    m = clamp(m, 0.0, 1.0);
    r = clamp(r, 0.0, 1.0);
}

// Environment specular sheen (option D). Real hair outdoors picks up a broad, soft
// highlight from the whole sky — not just the single point light. Hair here previously
// had only diffuse ambient (computeAmbient) plus one sharp direct specular, so it read
// crisp and unlike the skylit skin. This adds a view-dependent specular sheen sampled
// from the irradiance cube: because that cube is cosine-convolved it is inherently soft,
// so the sheen is a gentle skylit band that CANNOT clip into a hard white streak the way
// the direct R lobe does. It is purely additive lighting (no geometry/alpha/depth), so
// it cannot reintroduce the scalp bleed the coverage-alpha experiment caused.
vec3 computeEnvSheen(vec3 Tworld, vec3 Vworld, vec3 hairColor, float specular, float roughness) {
    if (!scene.useIBL)
        return vec3(0.0);

    // Strand-aware view-facing normal (perpendicular to the strand, toward the camera),
    // then reflect the view around it for the anisotropic specular lookup direction.
    vec3 N, B;
    buildBasis(Tworld, Vworld, N, B);
    vec3 R = reflect(-Vworld, N);

    // Match skybox / computeAmbient handedness so the sheen rotates with the sky.
    float rad  = radians(scene.envRotation);
    float c    = cos(rad);
    float s    = sin(rad);
    mat3  rotY = mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);

    vec3 env = texture(irradianceMap, normalize(rotY * R)).rgb * scene.ambientIntensity;

    // Grazing Fresnel (Schlick from the hair IOR): sheen strongest at glancing angles.
    float NoV  = clamp(dot(N, Vworld), 0.0, 1.0);
    float fres = fresnel(NoV, material.ior);

    // White primary highlight blended toward the hair colour (TRT-like tint), and a
    // small gloss boost so lower-roughness hair concentrates the sheen a touch more.
    vec3  tint  = mix(vec3(1.0), hairColor, 0.5);
    float gloss = clamp(1.0 - roughness, 0.0, 1.0);

    const float SHEEN_SCALE = 1.5; // overall strength knob for the env sheen

    return env * tint * (specular * fres) * (0.5 + 0.5 * gloss) * SHEEN_SCALE;
}

// MATTE model: no R, no TRT, no specular lobes at all — the production reading
// of an eyelash as a dark absorbing fiber rather than a glossy cylinder. Two
// terms replace the Marschner lobes:
//   - a WRAPPED Kajiya-Kay diffuse, so the fiber stays readable when the light
//     is off to the side instead of falling to black at grazing angles;
//   - a view-dependent forward-scatter term for light coming from behind the
//     fiber, which is the only "highlight" real lashes reliably show.
// Both are tinted by the absorption-derived base colour, so a darker lash stays
// dark instead of being brightened uniformly.
vec3 evalMatteLash(vec3 L, vec3 V, vec3 T, EpicHairBSDF b) {
    // Fiber "normal": the view projected perpendicular to the strand tangent —
    // the same construction evalKajiyaKayDiffuseAttenuation uses. Degenerate when
    // V is parallel to T, so fall back to an arbitrary perpendicular axis.
    vec3  raw     = V - T * dot(V, T);
    float rawLen2 = dot(raw, raw);
    vec3  N;
    if (rawLen2 < 1e-8)
    {
        vec3 fallback = abs(T.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        N             = normalize(fallback - T * dot(fallback, T));
    } else
    {
        N = normalize(raw);
    }

    const float WRAP = 1.0; // how far light bleeds past the fiber's terminator

    // `baseColor` is the melanin-derived SCATTERED colour of a fiber, not a
    // diffuse albedo — feeding it straight into a Lambert-ish lobe overshoots
    // badly and lands this model brighter and warmer than the Marschner path it
    // is supposed to calm down. This gain is the model's one art knob.
    const float MATTE_DIFFUSE_GAIN = 0.3;

    float NoL     = saturate((dot(N, L) + WRAP) / pow2(1.0 + WRAP));
    float kajiya  = 1.0 - abs(dot(T, L));
    float diffuse = MATTE_DIFFUSE_GAIN * (1.0 / PI) * mix(NoL, kajiya, 0.33);

    // Forward scatter: strongest when the light sits directly behind the fiber.
    float backlit = saturate(dot(-L, V));
    vec3  through = b.baseColor * b.TTpower * pow(backlit, 3.0) * (1.0 / PI);

    vec3 S = b.baseColor * diffuse + through;

    // Same energy bookkeeping the scattering branch of evalEyelashBSDF applies.
    // Without it this model ignores the dual-scattering attenuation the Marschner
    // path gets and lands BRIGHTER than the baseline it is meant to calm down.
    S = b.globalScattering * (S + b.localScattering) * b.opaqueVisibility;

    return max(S, vec3(0.0));
}

void main() {

    // BSDF setup ............................................................
    float melanin = material.baseColor.x;
    float redness = material.baseColor.y;

    // TODO: Check Unreal shader code to see what the exact tonemapping needed is
    // melanin = pow(melanin, 2.2);
    // redness = pow(redness, 2.2);
    
    applyNaturalVariation(melanin, redness, g_uv.x, g_uv.y);
    vec3 physicalSigma = getAbsorptionFromMelanin(melanin, redness, material.baseColor.z);
    vec3 epicBaseColor = hairAbsorptionToColor(physicalSigma);

    bsdf.baseColor = epicBaseColor;

    bsdf.roughness = material.roughness;
    bsdf.metallic  = material.metallic;
    bsdf.specular  = material.specular;

    bsdf.shift = material.shift;
    bsdf.ior   = material.ior;

    bsdf.Rpower   = material.Rpower;
    bsdf.TTpower  = material.TTpower;
    bsdf.TRTpower = material.TRTpower;

    int variant = int(material.variant);

    // TAPERED (fragment half — the geometry stage narrows the quad). The thin end
    // of a real lash is also more transmissive and less reflective than the base;
    // without this the tip is merely a finer dark line, not a translucent one.
    if (variant == VARIANT_TAPERED)
    {
        float tip = g_uv.x * material.tipTaper;
        bsdf.TTpower *= mix(1.0, 3.0, tip);
        bsdf.Rpower *= mix(1.0, 0.2, tip);
        bsdf.TRTpower *= mix(1.0, 0.2, tip);
    }

    bsdf.useLegacyAbsorption = (material.useLegacyAbsorption > 0.5);
    bsdf.useSeparableR       = (material.useSeparableR > 0.5);
    bsdf.useBacklit          = (material.useBacklit > 0.5);

    bsdf.clampBSDFValue = (material.clampBSDFValue > 0.5);

    bsdf.opaqueVisibility = material.opaqueVisibility;

    bsdf.localScattering  = vec3(0.0);
    bsdf.globalScattering = vec3(1.0);

    vec3 V = normalize(-g_pos);
    vec3 T = normalize(g_dir);
    if (material.useGlints > 0.5)
        T = computeMicroTangent(T, V, g_modelPos);

    // T = computeMicroTangent(T, N_orig, g_modelPos);

    // vec3  T         = computeGlintTangent();

    // DIRECT LIGHTING .......................................................
    vec3 color = vec3(0.0);
    for (int i = 0; i < scene.numLights; i++)
    {
        // If inside liught area influence
        if (isInAreaOfInfluence(scene.lights[i], g_pos))
        {

            vec3  shadow         = vec3(1.0);
            vec3  spread         = vec3(0.0);
            float directFraction = 1.0;
            float solidOcclusion = 1.0;
            if (int(object.otherParams.y) == 1 && scene.lights[i].shadowCast == 1)
            {
                if (scene.lights[i].shadowType == 0) // Classic
                    shadow = computeHairShadow(scene.lights[i], i, shadowMap, 0.7, g_modelPos, spread, directFraction);
                if (scene.lights[i].shadowType == 1) // VSM
                    shadow = computeHairShadow(scene.lights[i], i, shadowMap, 0.7, g_modelPos, spread, directFraction);

                // Hair-fiber transmittance above is calibrated for thin strands and barely
                // attenuates behind solid occluders. Multiply by the standard VSM Chebyshev
                // test so opaque casters (head, body, props) fully shadow hair.
                solidOcclusion  = computeVarianceShadow(shadowMap, scene.lights[i], i, g_modelPos);
                shadow         *= solidOcclusion;
                directFraction *= solidOcclusion;
            }

            // A DirectionalLight packs its *direction* (view space, w=0) into the
            // position slot, so it must not be treated as a point — doing so puts
            // the light one unit from the view origin, which collapses L onto V
            // (a headlight) and pins inBacklit at 0, silently killing the whole TT
            // lobe. Same branch physically_based.glsl already uses.
            vec3  L         = scene.lights[i].type != DIRECTIONAL_LIGHT
                                  ? normalize(scene.lights[i].position.xyz - g_pos)
                                  : normalize(scene.lights[i].position.xyz);
            float inBacklit = saturate(dot(-L, V));

            // Initialized: `visibility` is only assigned under advShadows but is
            // read unconditionally below, so with adv_shadows off this was an
            // uninitialized read feeding the scattering term.
            HairTransmittanceMask transMask;
            transMask.visibility = 1.0;
            if (material.advShadows > 0.0)
            {
                // Same directional-light caveat as L above: transform the stored
                // vector as a direction (w=0), not as a world point.
                vec3 coneDir = scene.lights[i].type != DIRECTIONAL_LIGHT
                                   ? normalize((camera.invView * vec4(scene.lights[i].position, 1.0)).xyz - g_modelPos)
                                   : normalize((camera.invView * vec4(scene.lights[i].position, 0.0)).xyz);
                transMask.visibility = computeHairShadowCone(
                    g_modelPos, coneDir, physicalSigma, material.shadowKnob );
            }
            // Fold solid-mesh occlusion into the transmittance mask so dual scattering
            // also sees the head/body — otherwise the scatter lobe lights hair through opaque casters.
            transMask.visibility *= solidOcclusion;
            float derivedHairCount = -log(max(transMask.visibility, 0.001));
            // Aplicas un factor para convertir "Densidad Óptica" a "Número de Capas" aproximado
            // Epic suele considerar que 1 unidad de HairCount es una capa de pelo visible.
            // Ajusta este 1.0/0.7 según tu densidad de voxelización original.
            // float densityScale  = 1.0 / 0.8;
            transMask.hairCount = derivedHairCount * material.scatterKnob * 1000.0;

            bsdf = evalHairMultipleScattering(V, L, T, transMask, hairLUT, bsdf);

            // Eyelash BSDF: same lobes as scalp hair but with Rpower/TTpower/
            // TRTpower/backlit wired in, so this asset can be tuned much less
            // reflective and more transmissive (see epic_hair_BSDF.glsl). MATTE
            // drops the Marschner lobes entirely for an absorbing-fiber model.
            vec3 bsdfValue;
            if (variant == VARIANT_MATTE)
                bsdfValue = evalMatteLash(L, V, T, bsdf) * directFraction;
            else
                bsdfValue = evalEyelashBSDF(L,
                                            V,
                                            T,
                                            directFraction,
                                            NpTex,
                                            bsdf,
                                            inBacklit,
                                            scene.lights[i].area,
                                            material.r > 0.5,
                                            material.tt > 0.5,
                                            material.trt > 0.5,
                                            material.scatter > 0.5);

            vec3 lighting = bsdfValue * scene.lights[i].color * scene.lights[i].intensity *
                            computeAttenuation(scene.lights[i], g_pos) * HAIR_GLOBAL_SCALE;

            color += lighting;
            // if(transMask.hairCount < 1000000.0)
            // color = vec3( transMask.visibility);
        }
    }

    // vec3 n1 = cross(g_modelDir, cross(camera.position.xyz, g_modelDir));
    vec3 fakeNormal = normalize(g_modelPos - object.volumeCenter);
    // vec3 fakeNormal = mix(n1,n2,0.5);

    // AMBIENT COMPONENT ..........................................................

    vec3 ambient = computeAmbient(fakeNormal, epicBaseColor);
    color += ambient;

    // Environment specular sheen: heavily damped for eyelashes. Scalp hair uses
    // this broad skylit glossy highlight at full strength, but on eyelashes it is
    // a big part of the "shiny wire" read — and because it carries its own
    // SHEEN_SCALE + gloss boost, lowering `specular` alone can't tame it. The damp
    // is now the material's `sheen_scale` (default 0.15, the old hardcoded value)
    // so its contribution can be judged without a recompile. MATTE has no
    // specular response by definition, so it takes none.
    if (variant != VARIANT_MATTE)
    {
        vec3 Vworld = normalize(camera.position.xyz - g_modelPos);
        color += computeEnvSheen(normalize(g_modelDir), Vworld, epicBaseColor, material.specular, material.roughness) *
                 material.sheenScale;
    }

    if (int(object.otherParams.x) == 1 && scene.enableFog)
    {
        float f = computeFog(gl_FragCoord.z);
        color   = f * color + (1 - f) * scene.fogColor.rgb;
    }

    // Defensive guard: the dual-scattering BSDF has multiple division-by-near-zero
    // sites (cosThetaD, af/ab weights, sigma_b denominators) that can still leak
    // NaN/Inf at degenerate strand/V/L alignments. Once NaN reaches fragColor it
    // poisons MSAA resolve and shows up as magenta — replace any non-finite value
    // with a safe black, and clamp legitimate HDR to a sane upper bound so a
    // single misbehaving lobe can't blow up bloom either.
    if (any(isnan(color)) || any(isinf(color)))
        color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(64.0));

    // Sub-pixel coverage is resolved by masking MSAA samples ourselves rather than
    // by fixed-function alpha-to-coverage. Alpha-to-coverage derives the sample
    // pattern from the alpha VALUE alone, so every overlapping fiber — all of which
    // carry nearly the same coverage — lands on the SAME subsample and the lash mass
    // never accumulates. At character framing a fiber is ~0.15 px, so that collapses
    // the whole lash line to 1/8 intensity and the lashes visually disappear.
    // Hashing the mask per fiber decorrelates them, so overlapping strands occupy
    // different samples and density builds up the way the geometry implies.
    if (variant == VARIANT_COVERAGE && g_coverage < 1.0)
    {
        int inMask  = gl_SampleMaskIn[0];
        int covered = bitCount(inMask);

        // Hashes are keyed on the per-strand random (g_uv.y / g_color) plus the pixel,
        // NOT on time or world position, so the pattern is stable frame to frame and
        // the lashes don't shimmer under animation.
        float h0 = hash31(vec3(gl_FragCoord.xy, g_uv.y * 131.0 + g_color.x * 71.0));
        float h1 = hash31(vec3(gl_FragCoord.yx, g_uv.y * 57.0 + g_color.x * 23.0));

        // Stochastic rounding: keeping floor(exact + rand) samples is unbiased on
        // average, where rounding to nearest would systematically over- or
        // under-cover fibers this thin.
        int keep = int(floor(g_coverage * float(covered) + h0));
        keep     = clamp(keep, 0, covered);

        int mask  = 0;
        int kept  = 0;
        int start = int(h1 * 32.0);
        for (int i = 0; i < 32 && kept < keep; ++i)
        {
            int bit = 1 << ((start + i) & 31);
            if ((inMask & bit) != 0)
            {
                mask |= bit;
                kept++;
            }
        }
        gl_SampleMask[0] = mask;
    }

    fragColor = vec4(color, 1.0);
    // fragColor = vec4(g_uv.x, 0.0,0.0, 1.0);
    // check whether result is higher than some threshold, if so, output as bloom threshold color
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    if (brightness > 1.0)
        outBrightColor = vec4(color, 1.0);
    else
        outBrightColor = vec4(0.0, 0.0, 0.0, 1.0);

    outNormals     = vec4(0.0);
    outAlbedoMask  = vec4(0.0, 0.0, 0.0, 0.0);
    outDiffuseIrr  = vec4(0.0);
    outBackIrr     = vec4(0.0);
    outLinearDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);
}