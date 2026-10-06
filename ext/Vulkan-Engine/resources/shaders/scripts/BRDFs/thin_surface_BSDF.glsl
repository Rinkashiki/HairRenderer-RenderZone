// =============================================================================
// Thin-surface BSDF (leaves, paper, thin foliage)
//
// Models a surface with no thickness that both reflects and transmits light,
// following the thin-surface mode of the Disney BSDF (Burley 2015):
//   - Reflection:   Cook-Torrance GGX specular + Hanrahan-Krueger diffuse.
//   - Transmission: light arriving from the back face, mirrored to the viewer's
//                   side and evaluated with the same lobes (GGX "specular
//                   transmission" + Hanrahan-Krueger "diffuse transmission").
//
// All vectors are expected in the same space (view space in leaf.glsl), and
// `normal` must point to the viewer's side of the surface.
//
// Requires fresnel.glsl (fresnelSchlick) to be included first. Defines the same
// GGX helpers as schlick_smith_BRDF.glsl, so never include both in one shader.
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
    float               specTrans;      // [0,1]  Fraction of light transmitted specularly (refraction)
    float               diffTrans;      // [0,2]  Diffuse transmission (1 = half reflected, half transmitted)
    vec3                transmittance;  // Tint applied to light crossing the surface (absorption)
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

// Disney (Burley 2012) Hanrahan-Krueger subsurface approximation.
// A diffuse lobe that flattens at grazing angles, mimicking light that scatters
// only a short distance below the surface.
//   NoL, NoV : cosines of the light / view directions with the normal. NOT
//              clamped here — the caller must pass values with NoL + NoV > 0,
//              otherwise 1/(NoL+NoV) is infinite.
//   LoH      : cosine between the light direction and the half vector.
//   roughness: perceptual roughness (not squared), as in Disney.
// Returns the BRDF value (albedo/PI · ss); the cosine factor is applied by the caller.
vec3 hanrahanKruegerDiffuse(float NoL, float NoV, float LoH, vec3 albedo, float roughness) {
    // Retro-reflection response at grazing angles
    float Fss90 = LoH * LoH * roughness; // perceptual roughness, as in Disney
    // Schlick-style blend from 1 (normal incidence) to Fss90 (grazing), for light and view
    float Fss   = mix(1.0, Fss90, pow(1.0 - NoL, 5.0)) * mix(1.0, Fss90, pow(1.0 - NoV, 5.0));
    // Hanrahan-Krueger shape: 1/(NoL+NoV) comes from single scattering in a
    // semi-infinite medium; 1.25 rescales it to preserve albedo.
    float ss    = 1.25 * (Fss * (1.0 / (NoL + NoV) - 0.5) + 0.5);
    return albedo / PI * ss;
}

// Evaluates the thin-surface BSDF for one light.
//   wi       : unit direction from the surface to the light
//   wo       : unit direction from the surface to the viewer
//   radiance : incoming radiance from the light (color · intensity · attenuation · shadow)
// Returns the outgoing radiance towards wo (reflected + transmitted), with the
// cosine factor already applied.
vec3 evalThinSurfaceBSDF(
    vec3 wi,
    vec3 wo,
    vec3 radiance,
    ThinSurfaceBSDF bsdf)
    {
    // NoL keeps its sign: > 0 means the light is on the front (reflection),
    // < 0 means it is behind the surface (transmission).
    float NoL = dot(bsdf.normal, wi);
    float NoV = max(dot(bsdf.normal, wo), 0.0);

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
    // Diffuse-reflection weight: energy not reflected specularly (1 - F), not
    // transmitted specularly (1 - specTrans), and not transmitted diffusely
    // (1 - diffTrans/2).
    vec3 weightDR = (vec3(1.0) - rF) * vec3(1.0 - bsdf.specTrans) * vec3(1.0 - bsdf.diffTrans / 2.0);

    vec3 rSpecular           = rNumerator / rDenominator;

    //Hanrahan Krueger diffuse
    float rLoH = dot(wi, rH) + EPSILON;
    vec3 rDiffuse = hanrahanKruegerDiffuse(NoL, NoV, rLoH, bsdf.albedo, bsdf.roughness);

    // max(NoL, 0) zeroes the reflection when the light is behind the surface
    vec3 reflection =(weightDR * rDiffuse + rSpecular) * radiance * max(NoL, 0.0);

    // ---------------------------------------------------------------------
    // Transmission (only contributes when NoL < 0)
    // ---------------------------------------------------------------------
    // Approximate energy transmitted from front to back face: light loses the
    // Fresnel-reflected part when entering the back face, and is tinted by
    // the surface's transmittance while crossing it.
    vec3 macroRF = fresnelSchlick(max(dot(wi, -bsdf.normal), 0.0), bsdf.F0);
    radiance *= bsdf.transmittance * (vec3(1.0) - macroRF);

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

    float tLoH = dot(wi, tH) + EPSILON;

    // Hanrahan Krueger diffuse, evaluated with the light mirrored onto the viewer's side
    // (abs(NoL) is the cosine of the mirrored light direction)
    vec3 tDiffuse = hanrahanKruegerDiffuse(abs(NoL), NoV, tLoH, bsdf.albedo, bsdf.roughness);

    // Specular-transmission weight
    vec3 weightST = vec3(bsdf.specTrans);

    // Diffuse-transmission weight: complement of weightDR's transmission factors
    vec3 weightDT = vec3(1.0 - bsdf.specTrans) * vec3(bsdf.diffTrans / 2.0);

    // max(-NoL, 0) zeroes the transmission when the light is in front of the surface
    vec3 transmission =(weightDT * tDiffuse + weightST * tSpecular) * radiance * max(-NoL, 0.0);

    return reflection + transmission;

}
