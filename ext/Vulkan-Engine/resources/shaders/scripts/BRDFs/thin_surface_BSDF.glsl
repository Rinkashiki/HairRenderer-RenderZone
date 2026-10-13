// =============================================================================
// Thin-surface BSDF (leaves, paper, thin foliage)
//
// Models a surface with no thickness that both reflects and transmits light,
// following the thin-surface mode of the Disney BSDF (Burley 2015):
//   - Reflection:   Cook-Torrance GGX specular + Lambert diffuse.
//   - Transmission: light arriving from the back face, scaled by specTrans,
//                   mirrored to the viewer's side and evaluated with the same
//                   lobes (GGX "specular transmission" + Lambert "diffuse
//                   transmission"), split between the two by diffTrans.
//
// All vectors are expected in the same space (view space in leaf.glsl), and
// `normal` must point to the viewer's side of the surface.
//
// Requires fresnel.glsl (fresnelSchlick) to be included first. Defines the same
// GGX helpers as schlick_smith_BRDF.glsl, so never include both in one shader.
// The previous, more elaborate version (Fresnel loss + tint, Hanrahan-Krueger
// diffuse, wrap lighting) is kept in thin_surface_BSDF_experimental.glsl.
// =============================================================================

#ifndef PI
#define PI              3.1415926535897932384626433832795
#endif
#ifndef EPSILON
#define EPSILON         0.001
#endif

// Per-fragment surface description, filled in by the calling shader.
struct                  ThinSurfaceBSDF{
    vec3                albedo;         // Base color (linear)
    float               opacity;        // Alpha, used by the caller for alpha test/blending
    vec3                normal;         // Shading normal, unit length, facing the viewer
    float               roughness;      // Perceptual roughness [0,1] (squared inside the GGX terms)
    float               ao;             // Ambient occlusion, applied by the caller to ambient light
    vec3                F0;             // Reflectance at normal incidence (dielectric: ~0.04)
    vec3                F;              // Unused, kept for parity with other BRDF structs
    vec3                specTrans;      // Per-channel fraction of back-face light that passes through the surface
    float               diffTrans;      // [0,2]  Diffuse share of the transmitted light, and the diffuse
                                        //        reflection it takes away (1 = half diffuse, half specular)
};

// Normal distribution function — Trowbridge-Reitz GGX.
// Density of microfacets oriented along the half vector H.
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a         = roughness * roughness;   // alpha = perceptual roughness²
    float a2        = a * a;
    float NdotH     = max(dot(N, H), 0.0);
    float NdotH2    = NdotH * NdotH;

    float num       = a2;
    float denom     = (NdotH2 * (a2 - 1.0) + 1.0);
    denom           = PI * denom * denom;

    return num / denom;
}

// Geometry (masking-shadowing) term for one direction — Schlick-GGX.
// k = (r+1)²/8 is the remapping recommended for direct (analytic) lights.
float geometrySchlickGGX(float NdotV, float roughness) {
    float r         = (roughness + 1.0);
    float k         = (r * r) / 8.0;

    float num       = NdotV;
    float denom     = NdotV * (1.0 - k) + k;

    return num / denom;
}

// Smith's method: total geometry term as the product of the view (masking)
// and light (shadowing) terms.
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV     = max(dot(N, V), 0.0);
    float NdotL     = max(dot(N, L), 0.0);
    float ggx2      = geometrySchlickGGX(NdotV, roughness);
    float ggx1      = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Evaluates the thin-surface BSDF for one light.
//   wi       : unit direction from the surface to the light
//   wo       : unit direction from the surface to the viewer
//   radiance : incoming radiance from the light (color · intensity · attenuation · shadow)
//   bsdf     : surface description
//   diffuseIrr : accumulated irradiance of the REFLECTED Lambert lobe (kD baked in, without
//                albedo/PI): (albedo/PI) * diffuseIrr == the reflected diffuse in the result.
//   backIrr    : accumulated irradiance of the TRANSMITTED Lambert lobe, same convention:
//                (albedo/PI) * backIrr == the transmitted diffuse in the result.
//   Both are read by the screen-space SSS pass, which blurs them separately (the
//   transmitted one wider); the caller zeroes them before the light loop.
// Returns the outgoing radiance towards wo (reflected + transmitted), with the
// cosine factor already applied.
vec3 evalThinSurfaceBSDF(
    vec3 wi,
    vec3 wo,
    vec3 radiance,
    ThinSurfaceBSDF bsdf,
    inout vec3 diffuseIrr,
    inout vec3 backIrr)
    {
    // NoL keeps its sign: > 0 means the light is on the front (reflection),
    // < 0 means it is behind the surface (transmission).
    float NoL = dot(bsdf.normal, wi);

    // Lambert diffuse, shared by reflection and transmission
    vec3 diffuse = bsdf.albedo / PI;

    // ---------------------------------------------------------------------
    // Reflection (only contributes when NoL >= 0)
    // ---------------------------------------------------------------------
	// Cook-Torrance BRDF: D · G · F / (4 · NoV · NoL)
    vec3 rH = normalize(wi + wo);
    float rNDF               = distributionGGX(bsdf.normal, rH, bsdf.roughness);
    float rG                 = geometrySmith(bsdf.normal, wo, wi, bsdf.roughness);
    vec3 rF = fresnelSchlick(max(dot(rH, wo), 0.0), bsdf.F0);

    vec3 rNumerator          = rNDF * rG * rF;
    float rDenominator       = 4.0 * max(dot(bsdf.normal, wo), 0.0) * max(dot(bsdf.normal, wi), 0.0) + 0.0001;
    vec3 rSpecular           = rNumerator / rDenominator;

    // Diffuse-reflection weight: energy not reflected specularly (1 - F) and not
    // transmitted diffusely (1 - diffTrans/2). Independent of specTrans.
    vec3 weightDR = (vec3(1.0) - rF) * vec3(1.0 - bsdf.diffTrans / 2.0);

    // max(NoL, 0) zeroes the reflection when the light is behind the surface
    vec3 reflection = (weightDR * diffuse + rSpecular) * radiance * max(NoL, 0.0);

    // Diffuse irradiance of the reflected Lambert lobe, for the SSS pass
    diffuseIrr += weightDR * radiance * max(NoL, 0.0);

    // ---------------------------------------------------------------------
    // Transmission (only contributes when NoL < 0)
    // ---------------------------------------------------------------------
    // specTrans alone decides how much light from the front face gets to the backface
    radiance *= bsdf.specTrans;

    // Mirror the light through the surface onto the viewer's side, so the
    // transmitted light can be evaluated with the same reflection-style lobes.
    wi = -wi;
    vec3 tH = normalize(wi + wo);

    // Specular transmission: GGX lobe around the mirrored light direction
    vec3 tF = fresnelSchlick(max(dot(tH, wo), 0.0), bsdf.F0);
    float tNDF               = distributionGGX(bsdf.normal, tH, bsdf.roughness);
    float tG                 = geometrySmith(bsdf.normal, wo, wi, bsdf.roughness);
    vec3 tNumerator          = tNDF * tG * tF;
    float tDenominator       = 4.0 * max(dot(bsdf.normal, wo), 0.0) * max(dot(bsdf.normal, wi), 0.0) + 0.0001;
    vec3 tSpecular           = tNumerator / tDenominator;

    // diffTrans splits the transmitted light between the diffuse and specular lobes
    vec3 weightDT = vec3(bsdf.diffTrans / 2.0);
    vec3 weightST = vec3(1.0 - bsdf.diffTrans / 2.0);

    // max(-NoL, 0) zeroes the transmission when the light is in front of the surface
    vec3 transmission = (weightDT * diffuse + weightST * tSpecular) * radiance * max(-NoL, 0.0);

    // Irradiance of the transmitted Lambert lobe (radiance already scaled by
    // specTrans), for the SSS pass, kept apart from the reflected one so it can be
    // blurred with a wider kernel. NoL makes only one of the two non-zero.
    backIrr += weightDT * radiance * max(-NoL, 0.0);

    return reflection + transmission;
}
