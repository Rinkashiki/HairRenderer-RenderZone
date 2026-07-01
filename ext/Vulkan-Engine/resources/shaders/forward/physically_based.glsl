#shader vertex
#version 460
#include camera.glsl
#include object.glsl


//Input
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec3 tangent;

//Output
layout(location = 0) out vec3 v_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec3 v_modelNormal;
layout(location = 3) out vec2 v_uv;
layout(location = 4) out vec3 v_modelPos;
layout(location = 5) out vec2 v_screenExtent;
layout(location = 6) out mat3 v_TBN;
// Pure object-space normal (no model matrix) — anchor for eye.glsl occlusion.
layout(location = 9) out vec3 v_objNormal;

//Uniforms
layout(set = 1, binding = 1) uniform MaterialUniforms {
    vec3    albedo;
    float   opacity;
    vec2    tileUV;
    bool    alphaTest;
    bool    blending;
    float   albedoWeight;
    float   metalness;
    float   metalnessWeight;
    float   roughness;
    float   roughnessWeight;
    float   occlusion;
    float   occlusionWeight;
    bool    hasAlbdoTexture;
    bool    hasNormalTexture;
    bool    hasRoughnessTexture;
    bool    hasMetallicTexture;
    bool    hasAOTexture;
    bool    hasMaskTexture;
    int     maskType;
    float   opacityWeight;
    bool    hasEmissiveTexture;
    vec3    emissiveColor;
    float   emissiveWeight;
    float   emissionIntensity;
    float   materialFlags;        // bit 0 = isReflective, bit 1 = hasScatteringTexture, bit 2 = hasClothesMaskTexture, bit 3 = hasEyeMaskTexture
    bool    hasCurvatureTexture;
    bool    hasBentNormalTexture;
    // slot9: detail params (tiling shared by normal & cavity, plus has-flags)
    float   detailTiling;
    float   detailNormalStrength;
    bool    hasDetailNormalTexture;
    bool    hasDetailCavityTexture;
    // slot10: cavity / dual-lobe scalars
    float   cavitySpecOcclusion;
    float   _slot10_pad;          // was cavitySSSAttenuation; SSS is cavity-agnostic now
    float   dualLobeMix;
    float   dualLobeRoughnessSoft;
    // slot11: Jimenez-style Disney sheen (peach fuzz)
    vec3    sheenColor;
    float   sheenIntensity;
} material;

void main() {

    gl_Position = camera.viewProj * object.model * vec4(pos, 1.0);

    v_uv = vec2(uv.x * material.tileUV.x, (1-uv.y) * material.tileUV.y);

    mat4 mv = camera.view * object.model;
    v_pos = (mv * vec4(pos, 1.0)).xyz;

    v_normal = normalize(mat3(transpose(inverse(mv))) * normal);

    if(material.hasNormalTexture || material.hasDetailNormalTexture) {
        // Tangent points along +U (as computed by compute_tangents_gram_smidt).
        // It must NOT be negated — a prior -T flipped the red/tangent axis and made
        // normal-mapped bumps read inverted.
        vec3 T = normalize(vec3(mv * vec4(tangent, 0.0)));
        vec3 N = normalize(vec3(mv * vec4(normal, 0.0)));
        // Bitangent as cross(T,N) (not cross(N,T)) reads the normal map's green
        // channel Y-down: the source maps are authored DirectX-style (Substance
        // default). Applies to base + detail normals (both route through v_TBN).
        vec3 B = cross(T, N);
        v_TBN = mat3(T, B, N);
    }

    v_modelPos = (object.model * vec4(pos, 1.0)).xyz;
    v_modelNormal = normalize(mat3(transpose(inverse(object.model))) * normal);
    v_objNormal = normalize(normal);

    v_screenExtent = camera.screenExtent;

}

#shader fragment
#version 460
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_ray_query : enable

#include camera.glsl
#include light.glsl
#include scene.glsl
#include object.glsl
#include utils.glsl
#include shadow_mapping.glsl
#include fresnel.glsl
#include ssao.glsl
#include IBL.glsl
#include reindhart.glsl
#include BRDFs/schlick_smith_BRDF.glsl
#include warp.glsl
#include raytracing.glsl
#include eye.glsl

//Input
layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_modelNormal;
layout(location = 3) in vec2 v_uv;
layout(location = 4) in vec3 v_modelPos;
layout(location = 5) in vec2 v_screenExtent;
layout(location = 6) in mat3 v_TBN;
layout(location = 9) in vec3 v_objNormal;

//Output
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outBrightColor;
layout(location = 2) out vec4 outNormals;
layout(location = 3) out vec4 outAlbedoMask;
layout(location = 4) out vec4 outDiffuseIrr;
layout(location = 5) out vec4 outBackIrr;
layout(location = 6) out vec4 outLinearDepth;


//Uniforms
layout(set = 0, binding = 2)    uniform sampler2DArray              shadowMap;
layout(set = 0, binding = 4)    uniform samplerCube                 irradianceMap;
layout(set = 0,  binding = 5)   uniform accelerationStructureEXT    TLAS;
layout(set = 0,  binding = 6)   uniform sampler2D                   blueNoiseMap;
// 5-pixel thin→thick scatter-distance LUT (sRGB-encoded). Mirrors the binding used
// in ssss.glsl so the d'Eon hybrid-normal blur biases stay aligned with the SSS
// pass's spectral profile.
layout(set = 0,  binding = 14)  uniform sampler2D                   scatterDistLUT;
layout(set = 1, binding = 1)    uniform MaterialUniforms {
    vec3    albedo;
    float   opacity;
    vec2    tileUV;
    bool    alphaTest;
    bool    blending;
    float   albedoWeight;
    float   metalness;
    float   metalnessWeight;
    float   roughness;
    float   roughnessWeight;
    float   occlusion;
    float   occlusionWeight;
    bool    hasAlbdoTexture;
    bool    hasNormalTexture;
    bool    hasRoughnessTexture;
    bool    hasMetallicTexture;
    bool    hasAOTexture;
    bool    hasMaskTexture;
    int     maskType;
    float   opacityWeight;
    bool    hasEmissiveTexture;
    vec3    emissiveColor;
    float   emissiveWeight;
    float   emissionIntensity;
    float   materialFlags;        // bit 0 = isReflective, bit 1 = hasScatteringTexture, bit 2 = hasClothesMaskTexture, bit 3 = hasEyeMaskTexture
    bool    hasCurvatureTexture;
    bool    hasBentNormalTexture;
    // slot9: detail params (tiling shared by normal & cavity, plus has-flags)
    float   detailTiling;
    float   detailNormalStrength;
    bool    hasDetailNormalTexture;
    bool    hasDetailCavityTexture;
    // slot10: cavity / dual-lobe scalars
    float   cavitySpecOcclusion;
    float   _slot10_pad;          // was cavitySSSAttenuation; SSS is cavity-agnostic now
    float   dualLobeMix;
    float   dualLobeRoughnessSoft;
    // slot11: Jimenez-style Disney sheen (peach fuzz)
    vec3    sheenColor;
    float   sheenIntensity;
} material;
layout(set = 2, binding = 0) uniform sampler2D albedoTex;
layout(set = 2, binding = 1) uniform sampler2D normalTex;
layout(set = 2, binding = 2) uniform sampler2D maskRoughTex;
layout(set = 2, binding = 3) uniform sampler2D metalTex;
layout(set = 2, binding = 4) uniform sampler2D occlusionTex;
layout(set = 2, binding = 5) uniform sampler2D emissiveTex;
layout(set = 2, binding = 6) uniform sampler2D bentNormalTex;
layout(set = 2, binding = 7) uniform sampler2D curvatureTex;
layout(set = 2, binding = 8) uniform sampler2D scatteringTex;
layout(set = 2, binding = 9) uniform sampler2D clothesMaskTex;
layout(set = 2, binding = 10) uniform sampler2D detailNormalTex;
layout(set = 2, binding = 11) uniform sampler2D detailCavityTex;
layout(set = 2, binding = 12) uniform sampler2D eyeMaskTex;


//BRDF Definiiton
SchlickSmithBRDF brdf;

// Per-channel diffuse shading normals (d'Eon/Hanrahan SIGGRAPH 2007 hybrid normals).
// Each RGB channel of the diffuse term integrates against a differently-blurred
// version of the detail normal: red sees the most blur (longest scattering mean
// free path in skin), blue the least. brdf.normal stays sharp (LOD 0) and is
// used by specular. On clothes (skinMask=0) all three collapse back to brdf.normal.
vec3 diffuseNormalWS_R;
vec3 diffuseNormalWS_G;
vec3 diffuseNormalWS_B;
// Macro normal (base normal map only, no detail). Used by the pre-integrated
// skin wrap and back-lighting — both operate at face-curvature scale where
// pore-level variation is meaningless.
vec3 smoothNormalWS;

// Skin coverage mask in [0,1]. 1 = skin, 0 = non-skin (clothes / eyes).
// Combines the clothes mask and the eye mask (white = excluded in both).
// Gates all skin-specific shading (microdetail, pre-integrated diffuse, SSS,
// sheen, dual-lobe, bent-normal IBL). Computed once in setupBRDFProperties()
// so the detail-normal build can also key off it.
float skinMask = 1.0;

const float distortion = 0.2;
const float backRadiancePower = 5.0;
const float backRadianceScale = 2.0;
const float ambient = 0.05;

// Penner-style pre-integrated skin diffuse (analytical Brisebois-Hoffman variant).
// curvature in [0,1]: 0 = flat surface (Lambert), 1 = highly curved (max wraparound).
// Returns per-channel wrapped NdotL response; red wraps farthest (longest mean free
// path in skin), green less, blue least -> warm halo at the terminator emerges
// without an explicit tint table.
vec3 preIntegratedSkinDiffuse(float NdotL, float curvature) {
    const vec3 channelWrap = vec3(1.0, 0.4, 0.2); // R/G/B relative scatter distances in skin
    vec3 w = 0.35 * curvature * channelWrap;
    vec3 wrapped = (vec3(NdotL) + w) / (vec3(1.0) + w);
    wrapped = max(wrapped, vec3(0.0));
    // energy normalize so a curvature=0, NdotL=1 surface stays at 1.0
    return wrapped / (vec3(1.0) + 0.5 * w);
}


void setupBRDFProperties(){
    // Skin coverage. Clothes mask and eye mask both use the white = excluded
    // convention, so skin survives only where BOTH are black. This gates the
    // microdetail build below and every skin-specific lighting term in main().
    int   _flags         = int(material.materialFlags);
    bool  _hasClothesMask = (_flags & 4) != 0;
    bool  _hasEyeMask     = (_flags & 8) != 0;
    skinMask = (_hasClothesMask ? (1.0 - texture(clothesMaskTex, v_uv).r) : 1.0)
             * (_hasEyeMask     ? (1.0 - texture(eyeMaskTex,     v_uv).r) : 1.0);

    // Microdetail (pore normal + cavity) is skin-only: fade its strength out on
    // clothes and eyes so brdf.normal collapses to the base/eye normal there.
    float effDetailStrength = material.detailNormalStrength * skinMask;

  //Setting input surface properties
    brdf.albedo = material.hasAlbdoTexture ? mix(material.albedo.rgb, texture(albedoTex, v_uv).rgb, material.albedoWeight) : material.albedo.rgb;
    brdf.opacity =  material.hasAlbdoTexture ?  mix(material.opacity, texture(albedoTex, v_uv).a, material.opacityWeight) :material.opacity;

    // Normal: if neither a base normal map nor a detail normal map is bound,
    // fall back to the vertex normal directly. Going through v_TBN when the
    // mesh lacks tangent data produces NaN (normalize of zero), which then
    // poisons all downstream shading.
    if (material.hasNormalTexture || material.hasDetailNormalTexture) {
        vec3 baseTangentN = material.hasNormalTexture
            ? (texture(normalTex, v_uv).rgb * 2.0 - 1.0)
            : vec3(0.0, 0.0, 1.0);
        smoothNormalWS = normalize(v_TBN * baseTangentN);

        // Whiteout blend: xy of base + xy of detail scaled by strength, z multiplied.
        // Robust at glancing angles, cheap, no NaN edges. We run it three more times
        // at per-channel mip-LOD bias to produce the d'Eon hybrid diffuse normals.
        vec3 detailTangentN = baseTangentN;
        if (material.hasDetailNormalTexture) {
            vec2 dUV = v_uv * material.detailTiling;
            // Sharp detail for specular (LOD 0).
            vec3 dN_spec = texture(detailNormalTex, dUV).rgb * 2.0 - 1.0;
            dN_spec.xy *= effDetailStrength;
            dN_spec.z   = max(dN_spec.z, 0.01);
            detailTangentN = normalize(vec3(baseTangentN.xy + dN_spec.xy,
                                            baseTangentN.z   * dN_spec.z));

            // Per-channel pre-blurred detail (R widest, B sharpest). Mip-LOD biases
            // are derived from the SSS scatter-distance LUT so the wavelength-graded
            // blur matches the same spectral profile used by the screen-space SSS
            // pass. Sample the LUT at mid-thickness (representative of skin overall),
            // linearize from sRGB, and map relative scatter distances to log-blur:
            //   bias_c = scale * log2(d_c / d_min)
            vec3 lutD = pow(texture(scatterDistLUT, vec2(0.5, 0.5)).rgb, vec3(2.2));
            float lutMin = max(min(min(lutD.r, lutD.g), lutD.b), 1e-4);
            vec3 detailBlurRGB = clamp(1.5 * log2(max(lutD, vec3(1e-4)) / lutMin),
                                       vec3(0.0), vec3(4.0));
            vec3 dN_r = textureLod(detailNormalTex, dUV, detailBlurRGB.r).rgb * 2.0 - 1.0;
            vec3 dN_g = textureLod(detailNormalTex, dUV, detailBlurRGB.g).rgb * 2.0 - 1.0;
            vec3 dN_b = textureLod(detailNormalTex, dUV, detailBlurRGB.b).rgb * 2.0 - 1.0;
            dN_r.xy *= effDetailStrength; dN_r.z = max(dN_r.z, 0.01);
            dN_g.xy *= effDetailStrength; dN_g.z = max(dN_g.z, 0.01);
            dN_b.xy *= effDetailStrength; dN_b.z = max(dN_b.z, 0.01);

            vec3 tN_r = normalize(vec3(baseTangentN.xy + dN_r.xy, baseTangentN.z * dN_r.z));
            vec3 tN_g = normalize(vec3(baseTangentN.xy + dN_g.xy, baseTangentN.z * dN_g.z));
            vec3 tN_b = normalize(vec3(baseTangentN.xy + dN_b.xy, baseTangentN.z * dN_b.z));
            diffuseNormalWS_R = normalize(v_TBN * tN_r);
            diffuseNormalWS_G = normalize(v_TBN * tN_g);
            diffuseNormalWS_B = normalize(v_TBN * tN_b);
        } else {
            vec3 baseWS       = normalize(v_TBN * baseTangentN);
            diffuseNormalWS_R = baseWS;
            diffuseNormalWS_G = baseWS;
            diffuseNormalWS_B = baseWS;
        }

        brdf.normal = normalize(v_TBN * detailTangentN);
    } else {
        brdf.normal       = v_normal;
        smoothNormalWS    = v_normal;
        diffuseNormalWS_R = v_normal;
        diffuseNormalWS_G = v_normal;
        diffuseNormalWS_B = v_normal;
    }

    if(material.hasMaskTexture) {
        // vec4 mask = pow(texture(maskRoughTex, v_uv).rgba, vec4(2.2)); //Correction linearize color
        vec4 mask = texture(maskRoughTex, v_uv).rgba; //Correction linearize color
        if(material.maskType == 0) { //HDRP UNITY
		    //Unity HDRP uses glossiness not roughness pipeline, so it has to be inversed
            brdf.roughness = 1.0 - mask.a;
            brdf.metalness = mask.r;
            brdf.ao = mask.g;
        } else if(material.maskType == 1) { //UNREAL
            brdf.roughness = mask.r;
            brdf.metalness = mask.b;
            brdf.ao = mask.g;
        } else if(material.maskType == 2) { //URP UNITY
            // TO DO ...
        }
    } else {
        brdf.roughness = material.hasRoughnessTexture ? mix(material.roughness, texture(maskRoughTex, v_uv).r, material.roughnessWeight) : material.roughness;
        brdf.metalness = material.hasMetallicTexture ? mix(material.metalness, texture(metalTex, v_uv).r, material.metalnessWeight) : material.metalness;
        brdf.ao = material.hasAOTexture ? mix(material.occlusion, texture(occlusionTex, v_uv).r, material.occlusionWeight) : material.occlusion;
    }
    brdf.F0 = vec3(0.04);
    brdf.F0 = mix(brdf.F0, brdf.albedo, brdf.metalness);

    brdf.emission =  material.hasEmissiveTexture ? mix(material.emissiveColor, texture(emissiveTex, v_uv).rgb, material.emissiveWeight) : material.emissiveColor;
    brdf.emission *= material.emissionIntensity;
}



void main() {

    // BRDF  ___________________________________________________________________
    setupBRDFProperties();

    if(material.alphaTest)
        if(brdf.opacity<1-EPSILON)discard;

    // skinMask (clothes + eye masks combined, white = excluded) was computed in
    // setupBRDFProperties() and gates all skin-specific effects below
    // (pre-integrated diffuse, bent-normal IBL, screen-space SSS, sheen).
    int   flags = int(material.materialFlags);
    bool  hasScatteringTexture  = (flags & 2) != 0;

    // Peach-fuzz sheen tint, derived from the SSS scatter-distance LUT instead of
    // an authored color: average the entire thin→thick ramp so the rim inherits
    // skin's overall subsurface character. Texels are linearized from sRGB and
    // averaged in linear space (averaging sRGB would bias the hue). Computed once
    // per fragment and only when the sheen is actually enabled.
    vec3 sheenTint = vec3(0.0);
    if (material.sheenIntensity > 0.0 && skinMask > 0.0) {
        for (int s = 0; s < 5; ++s)
            sheenTint += pow(texture(scatterDistLUT, vec2((float(s) + 0.5) / 5.0, 0.5)).rgb,
                             vec3(2.2));
        sheenTint *= 0.2; // average of the 5 LUT texels
    }

    //Compute all lights ___________________________________________________________________
    vec3 color = vec3(0.0);
    vec3 diffuseIrr = vec3(0.0);
    vec3 backIrr = vec3(0.0);
    for(int i = 0; i < scene.numLights; i++) {
        //If inside light area influence
        if(isInAreaOfInfluence(scene.lights[i], v_pos)){

            vec3 wi = scene.lights[i].type != DIRECTIONAL_LIGHT ? normalize(scene.lights[i].position - v_pos) : normalize(scene.lights[i].position.xyz);
            vec3 radiance = scene.lights[i].color * computeAttenuation(scene.lights[i], v_pos) * scene.lights[i].intensity;

            vec3 lighting = evalSchlickSmithBRDF(wi, normalize(-v_pos), radiance, brdf);

            float shadowFactor = 1.0;
            if(int(object.otherParams.y) == 1 && scene.lights[i].shadowCast == 1) {
                if(scene.lights[i].shadowType == 0) //Classic
                    shadowFactor = computeShadow(shadowMap, scene.lights[i], i, v_modelPos);
                if(scene.lights[i].shadowType == 1) //VSM
                    shadowFactor = computeVarianceShadow(shadowMap, scene.lights[i], i, v_modelPos);
                if(scene.lights[i].shadowType == 2) //Raytraced
                    shadowFactor = computeRaytracedShadow(
                        TLAS,
                        blueNoiseMap,
                        v_modelPos,
                        scene.lights[i].type != DIRECTIONAL_LIGHT ? scene.lights[i].shadowData.xyz - v_modelPos : scene.lights[i].shadowData.xyz,
                        int(scene.lights[i].shadowData.w),
                        scene.lights[i].area,
                        0);
            }
            lighting *= shadowFactor;

            // Diffuse/specular normal split with d'Eon hybrid normals:
            // - specular reflects off the sharp detailed microsurface (brdf.normal),
            // - each RGB diffuse channel integrates against a per-channel pre-blurred
            //   normal (red widest, blue sharpest) to mimic wavelength-dependent
            //   subsurface scattering at pore scale.
            // On clothes (skinMask=0), all three collapse to brdf.normal.
            vec3  dN_r = mix(brdf.normal, diffuseNormalWS_R, skinMask);
            vec3  dN_g = mix(brdf.normal, diffuseNormalWS_G, skinMask);
            vec3  dN_b = mix(brdf.normal, diffuseNormalWS_B, skinMask);
            vec3  NdotL_diff_rgb = max(vec3(dot(dN_r, wi), dot(dN_g, wi), dot(dN_b, wi)),
                                       vec3(0.0));
            float NdotL_smooth_raw = dot(smoothNormalWS, wi);
            float NdotL_spec       = max(dot(brdf.normal, wi), 0.0);
            vec3  wo = normalize(-v_pos);
            vec3  h  = normalize(wi + wo);
            vec3  F  = fresnelSchlick(max(dot(h, wo), 0.0), brdf.F0);
            vec3  kD = (vec3(1.0) - F) * (1.0 - brdf.metalness);

            // The diffuse term embedded in evalSchlickSmithBRDF uses brdf.normal
            // (detailed). Extract spec by subtracting that detailed diffuse, then
            // recompose with the per-channel hybrid diffuse we actually want.
            vec3 diffSpecPath = kD * brdf.albedo / PI * radiance * NdotL_spec * shadowFactor;
            vec3 specPart     = lighting - diffSpecPath;
            vec3 diffBase     = kD * brdf.albedo / PI * radiance * NdotL_diff_rgb * shadowFactor;

            // Dual-lobe specular (Penner GDC 2011): mix in a softer secondary
            // GGX lobe for the layered oily-on-dry skin highlight. Gated by
            // skinMask so clothes keep their single sharp lobe.
            float effectiveDualMix = material.dualLobeMix * skinMask;
            if (effectiveDualMix > 0.0) {
                SchlickSmithBRDF softBrdf = brdf;
                softBrdf.roughness        = material.dualLobeRoughnessSoft;
                vec3 lightingSoft = evalSchlickSmithBRDF(wi, wo, radiance, softBrdf) * shadowFactor;
                // Subtract the detailed-normal diffuse here too — both lighting
                // evaluations share brdf.normal, so their embedded diffuse matches.
                vec3 specSoft = lightingSoft - diffSpecPath;
                specPart = mix(specPart, specSoft, effectiveDualMix);
            }

            // Jimenez-style peach-fuzz sheen (Activision Digital Human, GDC 2013).
            // Additive view-grazing rim lobe:
            //   sheen = sheenTint * intensity * (1-NoV)^3 * NoL_smooth
            // sheenTint is the averaged SSS scatter-distance LUT color (above).
            // View-grazing (NoV) is what produces the silhouette glaze regardless
            // of light direction — half-angle Fresnel (Disney sheen) collapses to
            // zero whenever light and view are roughly aligned, which is the usual
            // inspection setup. NoL is taken against the macro (smooth) normal so
            // pore-level detail doesn't slice the sheen up. Gated by skinMask and
            // modulated by a curvature-derived fuzz mask so vellus distribution
            // reads correctly (more on nose/cheekbones/ears). Added into specPart
            // before the cavity multiply so pore crevices also dim the sheen.
            if (material.sheenIntensity > 0.0 && skinMask > 0.0) {
                float NoV      = max(dot(smoothNormalWS, wo), 0.0);
                float FV       = pow(1.0 - NoV, 3.0);
                float NoL_s    = max(dot(smoothNormalWS, wi), 0.0);
                float fuzzMask = material.hasCurvatureTexture
                    ? mix(0.3, 1.0, texture(curvatureTex, v_uv).r)
                    : 1.0;
                vec3  sheen = sheenTint * material.sheenIntensity
                            * FV * NoL_s * fuzzMask * skinMask
                            * radiance * shadowFactor;
                specPart += sheen;
            }

            // Cavity → specular occlusion: pore crevices receive less specular
            // (no shiny pores). Diffuse and SSS irradiance untouched.
            if (material.hasDetailCavityTexture) {
                float cavity = texture(detailCavityTex, v_uv * material.detailTiling).r;
                // skinMask fades the pore occlusion out on clothes and eyes.
                float cavOcc = mix(1.0, cavity, material.cavitySpecOcclusion * skinMask);
                specPart *= cavOcc;
            }

            lighting = diffBase + specPart;

            // Pre-integrated skin diffuse (Penner GDC 2011, analytical Brisebois):
            // replace the Lambertian diffuse term with a curvature-wrapped per-channel
            // response. Operates at face-curvature scale, so it uses the macro
            // (base-only) normal — independent of the d'Eon micro-blur above.
            //
            // diffIrrPerLight bakes in kD so the SSS pass can reconstruct the
            // exact diffuse contribution that landed in `color` via
            //   (albedo / PI) * diffIrr  ==  diffBase   (non-skin)
            //   (albedo / PI) * diffIrr  ==  diffWrapped (skin, curvature on)
            // letting `specular = hdr - localDiff` work without a max() clamp.
            vec3 diffIrrPerLight = kD * NdotL_diff_rgb * radiance * shadowFactor;
            if (material.hasCurvatureTexture && skinMask > 0.0)
            {
                float curvature   = texture(curvatureTex, v_uv).r;
                vec3  wrapped     = preIntegratedSkinDiffuse(NdotL_smooth_raw, curvature);
                vec3  diffWrapped = kD * brdf.albedo / PI * radiance * wrapped * shadowFactor;
                color += (diffWrapped - diffBase) * skinMask;
                diffIrrPerLight = mix(diffIrrPerLight, kD * wrapped * radiance * shadowFactor, skinMask);
            }

            color += lighting;
            diffuseIrr += diffIrrPerLight;

            vec3 HBack = wi + brdf.normal * distortion;
            float VDotH = pow(clamp(dot(normalize(-v_pos), -HBack), EPSILON, 1.0), backRadiancePower) * backRadianceScale;
            float attenuation = clamp(dot(brdf.normal, wi) + dot(normalize(-v_pos), -wi), EPSILON, 1.0);
            // radiance = scene.lights[i].color * attenuation * scene.lights[i].intensity;
            backIrr    += radiance * (VDotH + ambient);
        }
    }

    //Ambient component ___________________________________________________________________
    vec3 ambient;
    if(scene.useIBL){
        if (material.hasBentNormalTexture && skinMask > 0.0) {
            // Bent normal map is authored in tangent space, like a regular normal map.
            // Transform to world space via v_TBN and sample the irradiance cube along it.
            // Fresnel/specular still uses the smooth geometric normal so highlights stay sharp.
            vec3 bentNormalWS = normalize(v_TBN * (texture(bentNormalTex, v_uv).rgb * 2.0 - 1.0));
            vec3 bentAmbient = computeAmbientBentNormal(
                irradianceMap,
                scene.envRotation,
                v_modelNormal,
                bentNormalWS,
                normalize(camera.position.xyz-v_modelPos),
                brdf.albedo,
                brdf.F0,
                brdf.metalness,
                brdf.roughness,
                scene.ambientIntensity);
            vec3 plainAmbient = computeAmbient(
                irradianceMap,
                scene.envRotation,
                v_modelNormal,
                normalize(camera.position.xyz-v_modelPos),
                brdf.albedo,
                brdf.F0,
                brdf.metalness,
                brdf.roughness,
                scene.ambientIntensity);
            ambient = mix(plainAmbient, bentAmbient, skinMask);
        } else {
            ambient = computeAmbient(
                irradianceMap,
                scene.envRotation,
                v_modelNormal,
                normalize(camera.position.xyz-v_modelPos),
                brdf.albedo,
                brdf.F0,
                brdf.metalness,
                brdf.roughness,
                scene.ambientIntensity);
        }
    }else{
        ambient = (scene.ambientIntensity * scene.ambientColor) * brdf.albedo;
    }
    //Emission ___________________________________________________________________
    color += brdf.emission;
    //Ambient occlusion ___________________________________________________________________
    color += ambient * brdf.ao;
    // Mirror the direct-light pattern for ambient on skin: the IBL diffuse is in
    // `color` already (above), and we ALSO push the matching kD-baked irradiance
    // into diffuseIrr so the SSS post-process subtracts-and-replaces it the same
    // way it does for direct light. The `PI *` factor restores the 1/PI absorbed
    // by `localDiff = (albedo/PI)*diffIrr` on the SSS side, making the
    // reconstruction exact: (albedo/PI) * (PI * kdIrr * ao) == ambient * ao.
    // When SSS is off the SSS pass takes its early-out and the additive `color +=
    // ambient` above keeps the ambient visible.
    if (scene.useIBL && skinMask > 0.0) {
        vec3 V = normalize(camera.position.xyz - v_modelPos);
        vec3 kdIrr;
        if (material.hasBentNormalTexture) {
            vec3 bentNormalWS = normalize(v_TBN * (texture(bentNormalTex, v_uv).rgb * 2.0 - 1.0));
            kdIrr = computeAmbientKdIrradianceBent(
                irradianceMap, scene.envRotation, v_modelNormal, bentNormalWS, V,
                brdf.F0, brdf.metalness, brdf.roughness, scene.ambientIntensity);
        } else {
            kdIrr = computeAmbientKdIrradiance(
                irradianceMap, scene.envRotation, v_modelNormal, V,
                brdf.F0, brdf.metalness, brdf.roughness, scene.ambientIntensity);
        }
        diffuseIrr += PI * kdIrr * brdf.ao * skinMask;
    }
    //Eye socket occlusion (eye.glsl) — eye-mask fragments only _____________
    float eyeAmount = ((int(material.materialFlags) & 8) != 0) ? texture(eyeMaskTex, v_uv).r : 0.0;
    if (eyeAmount > 0.0)
        color = eye_shade(color, brdf.albedo, v_objNormal, eyeAmount);

    //Fog ___________________________________________________________________
    if(int(object.otherParams.x) == 1 && scene.enableFog) {
        float f = computeFog(gl_FragCoord.z);
        color = f * color + (1 - f) * scene.fogColor.rgb;
    }

    //Blending
    outColor = vec4(color, material.blending ? brdf.opacity: 1.0);

    // check whether result is higher than some threshold, if so, output as bloom threshold color
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    if(brightness > 1.0)
        outBrightColor = vec4(color, 1.0);
    else
        outBrightColor = vec4(0.0, 0.0, 0.0, 1.0);

    outNormals     = vec4(brdf.normal, 0.0);

    // Alpha carries the per-pixel SSS modulation mask: 1.0 = full subsurface
    // scattering (default for skin materials), 0.0 = pass-through (non-skin).
    // The scattering texture drives this per-texel so thin/translucent regions
    // like ears and nose tips scatter more than thick ones like forehead;
    // skinMask zeros it out on clothes regions (when clothes mask is bound).
    float scatterMask = hasScatteringTexture ? texture(scatteringTex, v_uv).r : 1.0;
    outAlbedoMask  = vec4(brdf.albedo, scatterMask * skinMask);

    // outDiffuseIrr.a is the skin gate consumed by ssss.glsl: 1.0 = full SSS
    // contribution, 0.0 = pixel excluded from the blur. We deliberately do NOT
    // attenuate by cavity here — light still enters and scatters laterally even
    // at the bottom of a pore, and over-weighting cavities broke the spectral
    // smoothness real skin shows. Cavity occlusion stays in the specular path
    // (cavitySpecOcclusion above) where the "no shiny pores" effect lives.
    outDiffuseIrr  = vec4(diffuseIrr, skinMask);
    outBackIrr     = vec4(backIrr, 0.0);
    outLinearDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);

}