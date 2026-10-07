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

// Receiver-side biased position (see above). Also returns the shadow texel
// footprint at the receiver (world units) and the PCF kernel half-extent (texels).
vec3 biasShadowReceiver(sampler2DArray shadowMap, LightUniform light, vec3 fragModelPos, vec3 N, vec3 L,
                        out float texelWorld, out float reach) {

    // Texel footprint at the receiver. The light view is rigid, so the length of
    // the combined matrix's first row recovers P[0][0] = 1/tan(fov/2).
    float w    = abs((light.viewProj * vec4(fragModelPos, 1.0)).w);
    float p00  = length(vec3(light.viewProj[0][0], light.viewProj[1][0], light.viewProj[2][0]));
    texelWorld = 2.0 * w / (p00 * float(textureSize(shadowMap, 0).x));

    // Kernel half-extent in texels (+0.5 for the bilinear fetch).
    reach = float(int(light.shadowData.w) / 2) * light.shadowData.y + 0.5;

    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    float sinT  = sqrt(1.0 - NdotL * NdotL);
    float tanT  = min(sinT / max(NdotL, 1e-3), SHADOW_MAX_SLOPE);

    return fragModelPos
         + N * (texelWorld * reach * sinT * SHADOW_NORMAL_OFFSET_SCALE)
         + L * (texelWorld * reach * tanT * SHADOW_SLOPE_SCALE);
}

float computeShadow(sampler2DArray shadowMap, LightUniform light, int lightId, vec3 fragModelPos, vec3 N, vec3 L) {
    float texelWorld, reach;
    vec3 biasedPos = biasShadowReceiver(shadowMap, light, fragModelPos, N, L, texelWorld, reach);
    return computeShadow(shadowMap, light, lightId, biasedPos);
}

// Lateral-scattering shadow for thin translucent surfaces lit from behind (leaves).
// Light entering the lit side of the surface diffuses sideways before exiting, so
// the visibility seen through the surface is the shadow map's visibility averaged
// around the receiver, weighted by Disney's normalized diffusion profile
//   R(r) = (e^(-r/d) + e^(-r/3d)) / (8*PI*d*r)
// The taps importance-sample its radial CDF F(r) = 1 - 1/4 e^(-r/d) - 3/4 e^(-r/3d)
// (truncated at 0.99), so every tap weighs 1/16. Offsets are in units of d, with
// golden-angle azimuths. Regenerate with:
//   r_i = F^-1((i + 0.5) / 16 * 0.99) (bisection, d = 1), angle_i = i * PI * (3 - sqrt(5))
const int  DIFFUSION_TAPS = 16;
const vec2 DIFFUSION_KERNEL[DIFFUSION_TAPS] = vec2[](
    vec2( 0.063183,  0.000000),  // r = 0.0632
    vec2(-0.146004,  0.133752),  // r = 0.1980
    vec2( 0.030220, -0.344345),  // r = 0.3457
    vec2( 0.309352,  0.403495),  // r = 0.5084
    vec2(-0.678637, -0.120041),  // r = 0.6892
    vec2( 0.752245, -0.478517),  // r = 0.8915
    vec2(-0.290843,  1.081922),  // r = 1.1203
    vec2(-0.636929, -1.226368),  // r = 1.3819
    vec2( 1.582717,  0.578006),  // r = 1.6850
    vec2(-1.887306,  0.779052),  // r = 2.0418
    vec2( 1.047088, -2.237566),  // r = 2.4704
    vec2( 0.897619,  2.861751),  // r = 2.9992
    vec2(-3.180661, -1.843258),  // r = 3.6762
    vec2( 4.487296, -0.986519),  // r = 4.5945
    vec2(-3.439276,  4.892016),  // r = 5.9800
    vec2(-1.121519, -8.654688)   // r = 8.7271
);

//   N: geometric world-space normal.  L: world-space direction TO the light.
//   scatterDistance: Disney's d, in world units.
//   rotation: per-fragment random angle (radians) applied to the whole kernel, so
//             neighbouring pixels use different tap directions — trades the fixed
//             pattern's banding for fine noise.
// Returns visibility (1 = lit), like computeShadow.
float computeShadowDiffused(sampler2DArray shadowMap, LightUniform light, int lightId, vec3 fragModelPos,
                            vec3 N, vec3 L, float scatterDistance, float rotation) {
    float texelWorld, reach;
    vec3 biasedPos = biasShadowReceiver(shadowMap, light, fragModelPos, N, L, texelWorld, reach);

    // Taps are laid out on the surface's tangent plane in world space and projected
    // individually, so a surface tilted relative to the light is handled correctly.
    vec3 T = normalize(abs(N.x) > 0.9 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(N, T);

    // Never narrower than the regular PCF footprint, so a tiny d doesn't alias.
    float scale = max(scatterDistance, texelWorld * reach);

    // Rotating every offset by the same angle keeps the radial distribution (and so
    // the profile weighting) intact; only the azimuths change.
    float c = cos(rotation), s = sin(rotation);
    mat2  kernelRotation = mat2(c, s, -s, c);

    float occlusion = 0.0;
    for(int i = 0; i < DIFFUSION_TAPS; ++i) {
        vec2 offset = kernelRotation * DIFFUSION_KERNEL[i];
        vec3 tapPos = biasedPos + (T * offset.x + B * offset.y) * scale;

        vec4 tapLightSpace = light.viewProj * vec4(tapPos, 1.0);
        vec3 coords        = tapLightSpace.xyz / tapLightSpace.w;
        coords.xy          = coords.xy * 0.5 + 0.5;
        if(coords.z > 1.0 || coords.z < 0.0)
            continue; // outside the light frustum: lit, same as computeShadow

        float occluderDepth = texture(shadowMap, vec3(coords.xy, lightId)).r;
        occlusion += coords.z - light.shadowData.x > occluderDepth ? 1.0 : 0.0;
    }
    return 1.0 - occlusion / float(DIFFUSION_TAPS);
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





