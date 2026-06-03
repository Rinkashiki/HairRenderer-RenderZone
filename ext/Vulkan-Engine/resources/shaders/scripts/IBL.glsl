mat3 rotationY(float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return mat3(c, 0.0, -s,
                0.0, 1.0, 0.0,
                s, 0.0, c);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}  
vec3 computeAmbient(samplerCube irradianceMap, float envRotation ,vec3 worldNormal, vec3 camPos, vec3 albedo, vec3 F0, float metalness, float roughness, float intensity){

    mat3 rotY = rotationY(radians(envRotation));
    vec3 rotatedNormal = normalize(rotY * worldNormal);

    vec3 specularity = fresnelSchlickRoughness(max(dot(rotatedNormal, camPos), 0.0), F0,roughness);
    vec3 aDiffuse = vec3(1.0)  - specularity;
    aDiffuse *= 1.0 - metalness;
    vec3 irradiance = texture(irradianceMap, rotatedNormal).rgb*intensity;

    vec3 diffuse = irradiance * albedo;
    return aDiffuse * diffuse;
}

// kD-baked irradiance helpers used by the SSS routing path. They return
// `aDiffuse * irradiance * intensity` (no albedo), so that `(albedo/PI) * (PI *
// kdIrr * ao)` reconstructs EXACTLY the ambient diffuse that was added into the
// HDR buffer by `computeAmbient(...) * ao`. With this invariant the SSS pass can
// subtract `localDiff` from `hdr` without a `max(...,0)` safety clamp.
vec3 computeAmbientKdIrradiance(samplerCube irradianceMap, float envRotation, vec3 worldNormal, vec3 camPos, vec3 F0, float metalness, float roughness, float intensity){
    mat3 rotY = rotationY(radians(envRotation));
    vec3 rotatedNormal = normalize(rotY * worldNormal);
    vec3 specularity = fresnelSchlickRoughness(max(dot(rotatedNormal, camPos), 0.0), F0, roughness);
    vec3 aDiffuse = (vec3(1.0) - specularity) * (1.0 - metalness);
    vec3 irradiance = texture(irradianceMap, rotatedNormal).rgb * intensity;
    return aDiffuse * irradiance;
}

// Bent variant: Fresnel uses the geometric normal (matches computeAmbientBentNormal).
vec3 computeAmbientKdIrradianceBent(samplerCube irradianceMap, float envRotation, vec3 worldNormal, vec3 bentNormal, vec3 camPos, vec3 F0, float metalness, float roughness, float intensity){
    mat3 rotY = rotationY(radians(envRotation));
    vec3 rotatedNormal     = normalize(rotY * worldNormal);
    vec3 rotatedBentNormal = normalize(rotY * bentNormal);
    vec3 specularity = fresnelSchlickRoughness(max(dot(rotatedNormal, camPos), 0.0), F0, roughness);
    vec3 aDiffuse = (vec3(1.0) - specularity) * (1.0 - metalness);
    vec3 irradiance = texture(irradianceMap, rotatedBentNormal).rgb * intensity;
    return aDiffuse * irradiance;
}

// Bent-normal variant: Fresnel uses the geometric normal, irradiance is sampled
// along the bent normal (average unoccluded direction). Lets crevices and under-
// hangs read incoming light from the direction it actually reaches them, instead
// of guessing from the surface orientation.
vec3 computeAmbientBentNormal(samplerCube irradianceMap, float envRotation, vec3 worldNormal, vec3 bentNormal, vec3 camPos, vec3 albedo, vec3 F0, float metalness, float roughness, float intensity){

    mat3 rotY = rotationY(radians(envRotation));
    vec3 rotatedNormal     = normalize(rotY * worldNormal);
    vec3 rotatedBentNormal = normalize(rotY * bentNormal);

    vec3 specularity = fresnelSchlickRoughness(max(dot(rotatedNormal, camPos), 0.0), F0, roughness);
    vec3 aDiffuse = vec3(1.0) - specularity;
    aDiffuse *= 1.0 - metalness;
    vec3 irradiance = texture(irradianceMap, rotatedBentNormal).rgb * intensity;

    vec3 diffuse = irradiance * albedo;
    return aDiffuse * diffuse;
}

