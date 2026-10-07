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
layout(location = 5) out mat3 v_TBN;

//Uniforms
layout(set = 1, binding = 1) uniform MaterialUniforms {
    // Mirrors LeafMaterial::get_uniforms (std140, one vec4 per line)
    vec3    albedo;
    float   opacity;
    vec2    tileUV;
    bool    alphaTest;
    bool    blending;
    float   albedoWeight;
    float   opacityWeight;
    float   roughness;
    float   roughnessWeight;
    float   occlusion;
    float   occlusionWeight;
    float   packedChannels;       // 2 bits per packed-atlas map: 0-1 rough, 2-3 AO
    float   _pad0;
    bool    hasAlbedoTexture;
    bool    hasNormalTexture;
    bool    hasRoughnessTexture;
    bool    hasAOTexture;
    vec3    transmittance;
    float   specTrans;
    float   diffTrans;
    float   scatterDistance;      // Disney diffusion d (world units) for back-lit shadows, 0 = off
} material;

void main() {

    gl_Position = camera.viewProj * object.model * vec4(pos, 1.0);

    v_uv = vec2(uv.x * material.tileUV.x, (1-uv.y) * material.tileUV.y);

    mat4 mv = camera.view * object.model;
    v_pos = (mv * vec4(pos, 1.0)).xyz;

    v_normal = normalize(mat3(transpose(inverse(mv))) * normal);

    if(material.hasNormalTexture) {
        // Tangent points along +U (as computed by compute_tangents_gram_smidt).
        vec3 T = normalize(vec3(mv * vec4(tangent, 0.0)));
        vec3 N = normalize(vec3(mv * vec4(normal, 0.0)));
        // Bitangent as cross(T,N): normal maps are authored DirectX-style (Y-down).
        vec3 B = cross(T, N);
        v_TBN = mat3(T, B, N);
    }

    v_modelPos = (object.model * vec4(pos, 1.0)).xyz;
    v_modelNormal = normalize(mat3(transpose(inverse(object.model))) * normal);

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
#include IBL.glsl
#include BRDFs/thin_surface_BSDF.glsl
#include warp.glsl
#include raytracing.glsl

//Input
layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_modelNormal;
layout(location = 3) in vec2 v_uv;
layout(location = 4) in vec3 v_modelPos;
layout(location = 5) in mat3 v_TBN;

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
    // Mirrors LeafMaterial::get_uniforms (std140, one vec4 per line)
    vec3    albedo;
    float   opacity;
    vec2    tileUV;
    bool    alphaTest;
    bool    blending;
    float   albedoWeight;
    float   opacityWeight;
    float   roughness;
    float   roughnessWeight;
    float   occlusion;
    float   occlusionWeight;
    float   packedChannels;       // 2 bits per packed-atlas map: 0-1 rough, 2-3 AO
    float   _pad0;
    bool    hasAlbedoTexture;
    bool    hasNormalTexture;
    bool    hasRoughnessTexture;
    bool    hasAOTexture;
    vec3    transmittance;
    float   specTrans;
    float   diffTrans;
    float   scatterDistance;      // Disney diffusion d (world units) for back-lit shadows, 0 = off
} material;
layout(set = 2, binding = 0) uniform sampler2D albedoTex;
layout(set = 2, binding = 1) uniform sampler2D normalTex;
layout(set = 2, binding = 2) uniform sampler2D roughnessTex;
layout(set = 2, binding = 3) uniform sampler2D occlusionTex;


const float LEAF_IOR = 1.5;

//BSDF Definition
ThinSurfaceBSDF bsdf;

void setupBSDFProperties(){

    //Setting input surface properties
    bsdf.albedo = material.hasAlbedoTexture ? mix(material.albedo.rgb, texture(albedoTex, v_uv).rgb, material.albedoWeight) : material.albedo.rgb;
    bsdf.opacity =  material.hasAlbedoTexture ?  mix(material.opacity, texture(albedoTex, v_uv).a, material.opacityWeight) :material.opacity;

    if (material.hasNormalTexture) {
        vec3 tangentN = texture(normalTex, v_uv).rgb * 2.0 - 1.0;
        bsdf.normal = normalize(v_TBN * tangentN);
    } else {
        bsdf.normal = normalize(v_normal);
    }

    // Roughness / AO may be channels of ONE packed image (ORM),
    // so each picks its own channel instead of always reading .r.
    int packed = int(material.packedChannels);
    bsdf.roughness = material.hasRoughnessTexture ? mix(material.roughness, texture(roughnessTex, v_uv)[packed & 3], material.roughnessWeight) : material.roughness;
    bsdf.ao = material.hasAOTexture ? mix(material.occlusion, texture(occlusionTex, v_uv)[(packed >> 2) & 3], material.occlusionWeight) : material.occlusion;

    // Dielectric
    float f0 = (LEAF_IOR - 1.0) / (LEAF_IOR + 1.0);
    bsdf.F0 = vec3(f0 * f0);

    bsdf.specTrans      = material.specTrans;
    bsdf.diffTrans      = material.diffTrans;
    bsdf.transmittance  = material.transmittance;
}



void main() {

    // BSDF  ___________________________________________________________________
    setupBSDFProperties();

    if(material.alphaTest)
        if(bsdf.opacity<1-EPSILON)discard;

    vec3 V = normalize(-v_pos);

    // Per-fragment random rotation of the back-lit shadow kernel (avoids banding).
    // Hashed from the pixel position, so it is stable from frame to frame.
    float kernelRotation = 2.0 * PI * whiteNoiseSample(vec3(gl_FragCoord.xy, 0.0)).x;

    //Compute all lights ___________________________________________________________________
    vec3 color = vec3(0.0);
    for(int i = 0; i < scene.numLights; i++) {
        //If inside light area influence
        if(isInAreaOfInfluence(scene.lights[i], v_pos)){

            vec3 wi = scene.lights[i].type != DIRECTIONAL_LIGHT ? normalize(scene.lights[i].position - v_pos) : normalize(scene.lights[i].position.xyz);
            vec3 radiance = scene.lights[i].color * computeAttenuation(scene.lights[i], v_pos) * scene.lights[i].intensity;

            float shadowFactor = 1.0;
            if(int(object.otherParams.y) == 1 && scene.lights[i].shadowCast == 1) {
                if(scene.lights[i].shadowType == 0) { //Classic (PCF + receiver-side biasing)
                    vec3 Lw = scene.lights[i].type != DIRECTIONAL_LIGHT
                            ? normalize((camera.invView * vec4(scene.lights[i].position.xyz, 1.0)).xyz - v_modelPos)
                            : normalize(mat3(camera.invView) * scene.lights[i].position.xyz);
                    // Back-lit: the transmitted light has diffused sideways through the
                    // blade, so soften the visibility with the diffusion kernel. 
                    // MAYBE IT WOULD BE BETTER TO NOT USE BRANCHING HERE AND INSTEAD MULTIPLY BY MAX(NOL, 0)
                    if(dot(bsdf.normal, wi) < 0.0 && material.scatterDistance > 0.0)
                        shadowFactor = computeShadowDiffused(shadowMap, scene.lights[i], i, v_modelPos, normalize(v_modelNormal), Lw, material.scatterDistance, kernelRotation);
                    else
                        shadowFactor = computeShadow(shadowMap, scene.lights[i], i, v_modelPos, normalize(v_modelNormal), Lw);
                }
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

            color += evalThinSurfaceBSDF(wi, V, radiance * shadowFactor, bsdf);
        }
    }

    //Ambient component ___________________________________________________________________
    vec3 ambient;
    if(scene.useIBL){
        ambient = computeAmbient(
            irradianceMap,
            scene.envRotation,
            v_modelNormal,
            normalize(camera.position.xyz-v_modelPos),
            bsdf.albedo,
            bsdf.F0,
            0.0,
            bsdf.roughness,
            scene.ambientIntensity);
    }else{
        ambient = (scene.ambientIntensity * scene.ambientColor) * bsdf.albedo;
    }
    //Ambient occlusion ___________________________________________________________________
    color += ambient * bsdf.ao;

    //Fog ___________________________________________________________________
    if(int(object.otherParams.x) == 1 && scene.enableFog) {
        float f = computeFog(gl_FragCoord.z);
        color = f * color + (1 - f) * scene.fogColor.rgb;
    }

    //Blending
    outColor = vec4(color, material.blending ? bsdf.opacity: 1.0);

    // check whether result is higher than some threshold, if so, output as bloom threshold color
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    if(brightness > 1.0)
        outBrightColor = vec4(color, 1.0);
    else
        outBrightColor = vec4(0.0, 0.0, 0.0, 1.0);

    outNormals     = vec4(bsdf.normal, 0.0);

    // Not skin: SSS pass-through (zero gates in the alpha channels).
    outAlbedoMask  = vec4(0.0);
    outDiffuseIrr  = vec4(0.0);
    outBackIrr     = vec4(0.0);
    outLinearDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);

}
