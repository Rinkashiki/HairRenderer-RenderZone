float filterPCF(sampler2DArray shadowMap ,int lightId ,int kernelSize, float extentMultiplier, vec3 coords, float bias) {

    int edge = kernelSize / 2;
    vec3 texelSize = 1.0 / textureSize(shadowMap, 0);

    float currentDepth = coords.z;

    float shadow = 0.0;

    for(int x = -edge; x <= edge; ++x) {
        for(int y = -edge; y <= edge; ++y) {
            float pcfDepth = texture(shadowMap, vec3(coords.xy + vec2(x, y) * texelSize.xy * extentMultiplier,lightId)).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    return shadow /= (kernelSize * kernelSize);

}

float computeShadow(sampler2DArray shadowMap ,LightUniform light, int lightId, vec3 fragModelPos) {

    vec4 pos_lightSpace = light.viewProj * vec4(fragModelPos, 1.0);

    vec3 projCoords = pos_lightSpace.xyz / pos_lightSpace.w;

    projCoords.xy  = projCoords.xy * 0.5 + 0.5;

    if(projCoords.z > 1.0 || projCoords.z < 0.0)
        return 1.0;
    
    return 1.0 - filterPCF(shadowMap, lightId,int(light.shadowData.w), light.shadowData.y, projCoords, light.shadowData.x);

}


// Classic PCF with receiver-side biasing (normal offset + slope-scaled offset).
//
// A constant depth bias alone cannot serve both cases at once: contact shadows
// (teeth behind lips, ~1 cm) need it tiny, while the 3x3 PCF kernel on a
// surface tilted away from the light needs it large — the taps one texel over
// sit closer to the light by texel*tan(theta), so a fixed fraction of the
// kernel reads "occluded" and every steep triangle darkens uniformly (the
// per-triangle patchwork). Both offsets here are expressed in WORLD units
// derived from the shadow texel footprint at the receiver, so they scale with
// light distance / fov / map resolution and `light.shadowData.x` can stay small.
//   N: geometric world-space normal.  L: world-space direction TO the light.
const float SHADOW_NORMAL_OFFSET_SCALE = 1.0;
const float SHADOW_SLOPE_SCALE         = 1.0;
const float SHADOW_MAX_SLOPE           = 8.0;   // tan(theta) clamp at grazing angles

float computeShadow(sampler2DArray shadowMap, LightUniform light, int lightId, vec3 fragModelPos, vec3 N, vec3 L) {

    // Texel footprint at the receiver. The light view is rigid, so the length of
    // the combined matrix's first row recovers P[0][0] = 1/tan(fov/2).
    float w          = abs((light.viewProj * vec4(fragModelPos, 1.0)).w);
    float p00        = length(vec3(light.viewProj[0][0], light.viewProj[1][0], light.viewProj[2][0]));
    float texelWorld = 2.0 * w / (p00 * float(textureSize(shadowMap, 0).x));

    // Kernel half-extent in texels (+0.5 for the bilinear fetch).
    float reach = float(int(light.shadowData.w) / 2) * light.shadowData.y + 0.5;

    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    float sinT  = sqrt(1.0 - NdotL * NdotL);
    float tanT  = min(sinT / max(NdotL, 1e-3), SHADOW_MAX_SLOPE);

    vec3 biasedPos = fragModelPos
                   + N * (texelWorld * reach * sinT * SHADOW_NORMAL_OFFSET_SCALE)
                   + L * (texelWorld * reach * tanT * SHADOW_SLOPE_SCALE);

    return computeShadow(shadowMap, light, lightId, biasedPos);
}

//Control light leaking
float linstep(float low, float high, float v){
    return clamp((v-low)/(high-low),0.0,1.0);
}

float computeVarianceShadow(sampler2DArray VSM ,LightUniform light, int lightId, vec3 fragModelPos) {

    vec4 pos_lightSpace = light.viewProj * vec4(fragModelPos, 1.0);

    vec3 projCoords = pos_lightSpace.xyz / pos_lightSpace.w;

    projCoords.xy  = projCoords.xy * 0.5 + 0.5;

    // ChebyshevUpperBound {
    vec2 moments = texture(VSM, vec3(projCoords.xy,lightId)).rg;
    float p = step(projCoords.z,moments.x);
    float variance = max(moments.y-moments.x*moments.x,0.00002);

    float d = projCoords.z - moments.x;
    float pMax = linstep(light.shadowData.x,1.0,variance / (variance + d*d));
    //}

    if(projCoords.z > 1.0 || projCoords.z < 0.0)
        return 1.0;

    return min(max(p,pMax),1.0);

}





