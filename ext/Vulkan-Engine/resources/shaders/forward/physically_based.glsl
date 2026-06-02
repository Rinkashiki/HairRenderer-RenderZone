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
    float   materialFlags;        // bit 0 = isReflective, bit 1 = hasScatteringTexture, bit 2 = hasClothesMaskTexture
    bool    hasCurvatureTexture;
    bool    hasBentNormalTexture;
    // slot9: detail params (tiling shared by normal & cavity, plus has-flags)
    float   detailTiling;
    float   detailNormalStrength;
    bool    hasDetailNormalTexture;
    bool    hasDetailCavityTexture;
    // slot10: cavity / dual-lobe scalars
    float   cavitySpecOcclusion;
    float   cavitySSSAttenuation; // reserved (step 11)
    float   dualLobeMix;          // reserved (step 10)
    float   dualLobeRoughnessSoft;// reserved (step 10)
} material;

void main() {

    gl_Position = camera.viewProj * object.model * vec4(pos, 1.0);

    v_uv = vec2(uv.x * material.tileUV.x, (1-uv.y) * material.tileUV.y);

    mat4 mv = camera.view * object.model;
    v_pos = (mv * vec4(pos, 1.0)).xyz;

    v_normal = normalize(mat3(transpose(inverse(mv))) * normal);

    if(material.hasNormalTexture) {
        vec3 T = -normalize(vec3(mv * vec4(tangent, 0.0)));
        vec3 N = normalize(vec3(mv * vec4(normal, 0.0)));
        vec3 B = cross(N, T);
        v_TBN = mat3(T, B, N);
    }

    v_modelPos = (object.model * vec4(pos, 1.0)).xyz;
    v_modelNormal = normalize(mat3(transpose(inverse(object.model))) * normal);

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

//Input
layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_modelNormal;
layout(location = 3) in vec2 v_uv;
layout(location = 4) in vec3 v_modelPos;
layout(location = 5) in vec2 v_screenExtent;
layout(location = 6) in mat3 v_TBN;

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
    float   materialFlags;        // bit 0 = isReflective, bit 1 = hasScatteringTexture, bit 2 = hasClothesMaskTexture
    bool    hasCurvatureTexture;
    bool    hasBentNormalTexture;
    // slot9: detail params (tiling shared by normal & cavity, plus has-flags)
    float   detailTiling;
    float   detailNormalStrength;
    bool    hasDetailNormalTexture;
    bool    hasDetailCavityTexture;
    // slot10: cavity / dual-lobe scalars
    float   cavitySpecOcclusion;
    float   cavitySSSAttenuation; // reserved (step 11)
    float   dualLobeMix;          // reserved (step 10)
    float   dualLobeRoughnessSoft;// reserved (step 10)
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


//BRDF Definiiton
SchlickSmithBRDF brdf;

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

        // Detail/pore normal: high-frequency tileable normal map blended on top
        // via "whiteout" blend (xy of base + xy of detail scaled by strength,
        // z multiplied). Robust to glancing angles, cheap, no NaN edges.
        if (material.hasDetailNormalTexture) {
            vec3 detailN = texture(detailNormalTex, v_uv * material.detailTiling).rgb * 2.0 - 1.0;
            detailN.xy *= material.detailNormalStrength;
            detailN.z = max(detailN.z, 0.01);
            baseTangentN = normalize(vec3(baseTangentN.xy + detailN.xy,
                                          baseTangentN.z   * detailN.z));
        }

        brdf.normal = normalize(v_TBN * baseTangentN);
    } else {
        brdf.normal = v_normal;
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

    // Clothes mask gates all skin-specific effects (pre-integrated diffuse,
    // bent-normal IBL, screen-space SSS). Convention: white = clothes, black = skin.
    int   flags = int(material.materialFlags);
    bool  hasScatteringTexture  = (flags & 2) != 0;
    bool  hasClothesMaskTexture = (flags & 4) != 0;
    float skinMask = hasClothesMaskTexture ? (1.0 - texture(clothesMaskTex, v_uv).r) : 1.0;

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

            // Common diffuse / specular split. Used by:
            //   - Curvature wrap (step 3): replaces Lambert diffuse with pre-integrated.
            //   - Cavity specular occlusion (step 9): attenuates the specular part only.
            // Cheaper than two separate Fresnel evaluations.
            float NdotL_raw = dot(brdf.normal, wi);
            float NdotL     = max(NdotL_raw, 0.0);
            vec3  wo = normalize(-v_pos);
            vec3  h  = normalize(wi + wo);
            vec3  F  = fresnelSchlick(max(dot(h, wo), 0.0), brdf.F0);
            vec3  kD = (vec3(1.0) - F) * (1.0 - brdf.metalness);
            vec3  diffBase = kD * brdf.albedo / PI * radiance * NdotL * shadowFactor;
            vec3  specPart = lighting - diffBase;

            // Dual-lobe specular (Penner GDC 2011): mix in a softer secondary
            // GGX lobe for the layered oily-on-dry skin highlight. Gated by
            // skinMask so clothes keep their single sharp lobe.
            float effectiveDualMix = material.dualLobeMix * skinMask;
            if (effectiveDualMix > 0.0) {
                SchlickSmithBRDF softBrdf = brdf;
                softBrdf.roughness        = material.dualLobeRoughnessSoft;
                vec3 lightingSoft = evalSchlickSmithBRDF(wi, wo, radiance, softBrdf) * shadowFactor;
                // diffBase is identical for both calls (kD depends on F, not roughness)
                vec3 specSoft = lightingSoft - diffBase;
                specPart = mix(specPart, specSoft, effectiveDualMix);
                lighting = diffBase + specPart;
            }

            // Cavity → specular occlusion: pore crevices receive less specular
            // (no shiny pores). Diffuse and SSS irradiance untouched.
            if (material.hasDetailCavityTexture) {
                float cavity = texture(detailCavityTex, v_uv * material.detailTiling).r;
                float cavOcc = mix(1.0, cavity, material.cavitySpecOcclusion);
                specPart *= cavOcc;
                lighting = diffBase + specPart;
            }

            // Pre-integrated skin diffuse (Penner GDC 2011, analytical Brisebois):
            // replace the Lambertian diffuse term with a curvature-wrapped per-channel
            // response. Specular is left untouched.
            vec3 diffIrrPerLight = vec3(NdotL) * radiance * shadowFactor;
            if (material.hasCurvatureTexture && skinMask > 0.0)
            {
                float curvature   = texture(curvatureTex, v_uv).r;
                vec3  wrapped     = preIntegratedSkinDiffuse(NdotL_raw, curvature);
                vec3  diffWrapped = kD * brdf.albedo / PI * radiance * wrapped * shadowFactor;
                color += (diffWrapped - diffBase) * skinMask;
                diffIrrPerLight = mix(diffIrrPerLight, wrapped * radiance * shadowFactor, skinMask);
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

    // outDiffuseIrr.a carries the per-pixel SSS *sample weight* — used by the
    // SSS post-process to attenuate this pixel's contribution when neighbouring
    // pixels integrate over it. Cavity drives down the weight inside pore
    // crevices, so scattered light no longer bleeds across pore boundaries.
    // Non-skin pixels (clothes via skinMask, hair/sky in their own shaders)
    // write 0 here, which excludes them from the SSS blur entirely.
    float sssSampleWeight = skinMask;
    if (material.hasDetailCavityTexture) {
        float cavity = texture(detailCavityTex, v_uv * material.detailTiling).r;
        sssSampleWeight *= mix(1.0, cavity, material.cavitySSSAttenuation);
    }
    outDiffuseIrr  = vec4(diffuseIrr, sssSampleWeight);
    outBackIrr     = vec4(backIrr, 0.0);
    outLinearDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);

}