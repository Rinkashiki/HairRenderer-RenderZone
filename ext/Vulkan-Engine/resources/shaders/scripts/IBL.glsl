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

