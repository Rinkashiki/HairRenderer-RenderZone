
/////////////////////////////////////////////
// EPIC'S UNREAL REAL-TIME ARTIST-FRIENDLY FIT
/////////////////////////////////////////////

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#define ONE_OVER_PI (1.0 / PI)
#define ONE_OVER_PI_HALF (2.0 / PI)
#define DEG2RAD(x) ((x) / 180.0 * PI)
#define HAIR_GLOBAL_SCALE 2.0

// #define TRANS_MARSHNER
#define TRANS_EPIC
// #define TRANS_PHARR
#define HAIR_REFERENCE 0

// Hair reflectance component (R, TT, TRT, Local Scattering, Global Scattering, Multi Scattering,...)

struct EpicHairBSDF {

    vec3 baseColor;

    float roughness;
    float metallic;
    float specular;

    float shift;
    float ior;

    float Rpower;
    float TTpower;
    float TRTpower;

    bool useLegacyAbsorption;
    bool useSeparableR;
    bool useBacklit;

    bool clampBSDFValue;

    float opaqueVisibility;

    vec3 localScattering;
    vec3 globalScattering;
};

struct HairAverageScattering {
    vec3 A_back;
    vec3 A_front;
};

struct HairTransmittanceMask {
    float visibility;
    float hairCount;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Utility functions
///////////////////////////////////////////////////////////////////////////////////////////////////

vec3 hairColorToAbsorption(vec3 C) {
    const float B  = 0.3;
    const float b2 = B * B;
    const float b3 = B * b2;
    const float b4 = b2 * b2;
    const float b5 = B * b4;
    const float D  = (5.969 - 0.215 * B + 2.532 * b2 - 10.73 * b3 + 5.574 * b4 + 0.245 * b5);

    vec3 L = log(max(C, vec3(1e-6))) / D; // protección contra log(0)

    return L * L; // evita pow(vec3,vec3)

    // return -log(max(C, vec3(1e-4)));
}


vec3 hairAbsorptionToColor(vec3 A) {
    const float B = 0.3;
    const float b2 = B * B;
    const float b3 = B * b2;
    const float b4 = b2 * b2;
    const float b5 = B * b4;
    const float D = (5.969 - 0.215 * B + 2.532 * b2 - 10.73 * b3 + 5.574 * b4 + 0.245 * b5);
    
    // Epic usa: exp(-sqrt(A) * D)
    // El sqrt aquí se cancela con el L*L de la función inversa
    return exp(-sqrt(max(A, vec3(0.0))) * D);
}




vec3 getAbsorptionFromMelanin(float m, float r, float scale) {
   // Safety clamp
    float m_safe = clamp(m, 0.0001, 1.0);

    float base_melanin = -log(max(1.0 - m_safe, 0.0001)) * 0.35;

    float density_boost = 1.0 + (r * 1.2);
    float total_melanin = base_melanin * density_boost;

    float eumelanin = total_melanin * (1.0 - r);
    float pheomelanin = total_melanin * r;

    
    // Eumelanina (Marrón Estándar)
    vec3 eu_color = vec3(0.506, 0.841, 1.653);

    // Feomelanina (Rojo "Deep Ginger" Tweaked)
    vec3 pheo_color = vec3(0.150, 0.800, 2.500);

    // 5. Combinación Lineal
    return (eumelanin * eu_color) + (pheomelanin * pheo_color);
}


float g(float theta, float beta, bool bClampBSDFValue) {
    // Clamp beta for the denominator term, as otherwise the Gaussian normalization returns too high value.
    // This clamps allow to prevent large value for low roughness, while keeping the highlight shape/sharpness
    // similar.
    const float DenominatorB = bClampBSDFValue ? max(beta, 0.01) : beta;
    return exp(-0.5 * pow2(theta) / (beta * beta)) / (sqrt(2 * PI) * DenominatorB); // No unit-height
}
float g2(float theta, float beta) {
    // const float A = 1.f / sqrt(2 * PI * Variance);
    const float A = 1.;
    return A * exp(-0.5 * pow2(theta) / beta);
}

float fresnel(float cosTheta, float ior) {
    const float n  = ior;
    const float F0 = pow2((1 - n) / (1 + n));
    return F0 + (1 - F0) * pow(1 - cosTheta, 5.0);
}

vec3 evalKajiyaKayDiffuseAttenuation(vec3 color, float metallic, vec3 L, vec3 V, vec3 N, float shadow) {
    float kajiyaDiffuse = 1.0 - abs(dot(N, L));

    // V projected perpendicular to N (here N is actually the strand tangent T).
    // Degenerate at V parallel to T — projection is zero, normalize → NaN. Fall
    // back to a stable arbitrary perpendicular axis in that case.
    vec3 raw = V - N * dot(V, N);
    float rawLen2 = dot(raw, raw);
    vec3 fakeNormal;
    if (rawLen2 < 1e-8) {
        vec3 fallback = abs(N.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        fakeNormal = normalize(fallback - N * dot(fallback, N));
    } else {
        fakeNormal = normalize(raw);
    }
    // N = normalize( DiffuseN + FakeNormal * 2 );
    N = fakeNormal;

    // Hack approximation for multiple scattering.
    float minValue       = 0.0001;
    float wrap           = 1.0;
    float NoL            = saturate((dot(N, L) + wrap) / pow2(1.0 + wrap));
    float diffuseScatter = (1 / PI) * mix(NoL, kajiyaDiffuse, 0.33) * metallic;
    float luma           = luminance(color);
    vec3  baseOverLuma   = abs(color / max(luma, minValue));
    vec3  scatterTint    = shadow < 1.0 ? pow(baseOverLuma, vec3(1.0 - shadow)) : vec3(1.0);
    return sqrt(abs(color)) * diffuseScatter * scatterTint;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

#if HAIR_REFERENCE

struct HairTempData {
    float SinThetaL;
    float SinThetaV;
    float CosThetaD;
    float CosThetaT;
    float CosPhi;
    float CosHalfPhi;
    float VoL;
    float n_prime;
};

// ==========================================================
// Modified Bessel function I0
// ==========================================================
float I0(float x) {
    x = abs(x);
    float a;

    if (x < 3.75)
    {
        float t  = x / 3.75;
        float t2 = t * t;

        a = +0.0045813;
        a = a * t2 + 0.0360768;
        a = a * t2 + 0.2659732;
        a = a * t2 + 1.2067492;
        a = a * t2 + 3.0899424;
        a = a * t2 + 3.5156229;
        a = a * t2 + 1.0;
    } else
    {
        float t = 3.75 / x;

        a = +0.00392377;
        a = a * t - 0.01647633;
        a = a * t + 0.02635537;
        a = a * t - 0.02057706;
        a = a * t + 0.00916281;
        a = a * t - 0.00157565;
        a = a * t + 0.00225319;
        a = a * t + 0.01328592;
        a = a * t + 0.39894228;

        a *= exp(x) * inversesqrt(x); // rsqrt → inversesqrt
    }

    return a;
}

// ==========================================================
// Longitudinal Scattering
// ==========================================================
float longitudinalScattering(float B, float SinThetaL, float SinThetaV) {
    float v = B * B;

    float CosThetaL2 = 1.0 - SinThetaL * SinThetaL;
    float CosThetaV2 = 1.0 - SinThetaV * SinThetaV;

    float Mp = 0.0;

    if (v < 0.1)
    {
        float a = sqrt(CosThetaL2 * CosThetaV2) / v;
        float b = -SinThetaL * SinThetaV / v;

        float logI0a = (a > 12.0) ? (a + 0.5 * (-log(2.0 * PI) + log(1.0 / a) + 0.125 / a)) : log(I0(a));

        Mp = exp(logI0a + b - (1.0 / v) + 0.6931 + log(0.5 / v));
    } else
    {
        Mp = (1.0 / (exp(2.0 / v) * v - v)) * exp((1.0 - SinThetaL * SinThetaV) / v) * I0(sqrt(CosThetaL2 * CosThetaV2) / v);
    }

    return Mp;
}

// ==========================================================
// Gaussian Detector
// ==========================================================
float gaussianDetector(float Bp, float Phi) {
    float Dp = 0.0;

    for (int k = -4; k <= 4; k++)
    {
        Dp += g(Bp, Phi - (2.0 * PI) * float(k), false);
    }

    return Dp;
}

// ==========================================================
// Attenuation
// ==========================================================
vec3 attenuation(uint p, float h, vec3 Color, HairTempData HairTemp) {
    vec3 A;

    if (p == 0u)
    {
        A = vec3(fresnel(sqrt(0.5 + 0.5 * HairTemp.VoL), 1.55));
    } else
    {
        vec3 Sigma_ae = vec3(0.419, 0.697, 1.37);
        vec3 Sigma_ap = vec3(0.187, 0.4, 1.05);

        vec3 ua       = -0.25 * log(Color); // Absorption
        vec3 ua_prime = ua / HairTemp.CosThetaT;

        float yi = asin(h);
        float yt = asin(h / HairTemp.n_prime);

        float f = fresnel(HairTemp.CosThetaD * sqrt(1.0 - h * h), 1.55);
        vec3  T = exp(-2.0 * ua_prime * cos(yt));

        if (p == 1u)
            A = pow(1.0 - f, 2.0) * T;
        else
            A = pow(1.0 - f, 2.0) * f * (T * T);
    }

    return A;
}

// ==========================================================
// Omega
// ==========================================================
float omega(uint p, float h, HairTempData HairTemp) {
    float yi = asin(h);
    float yt = asin(h / HairTemp.n_prime);

    return 2.0 * float(p) * yt - 2.0 * yi + float(p) * PI;
}

// Sample Precomputed Far-Field tex
vec3 sampleN(uint p, sampler3D NpTex, HairTempData HairTemp) {
    vec3  Np  = vec3(0.0);
    float phi = acos(HairTemp.CosPhi);
    if (p == 0)
        Np = texture(NpTex, vec3(phi * ONE_OVER_PI, HairTemp.CosThetaD, 0.3)).rrr;
    if (p == 1)
        Np = texture(NpTex, vec3(phi * ONE_OVER_PI, HairTemp.CosThetaD, 0.3)).ggg;
    if (p == 2)
        Np = texture(NpTex, vec3(phi * ONE_OVER_PI, HairTemp.CosThetaD, 0.3)).bbb;

    return clamp(Np, 0.0, 1.0);
}

// ==========================================================
// Azimuthal Scattering
// ==========================================================
vec3 azimuthalScattering(uint p, float Bp, vec3 Color, HairTempData HairTemp, sampler3D NpTex, uvec2 Random) {
    // float Phi = acos(HairTemp.CosPhi);

    // float Offset = float(Random.x & 0xffffu) / float(1u << 16);

    // const uint Num = 64u;

    // vec3 Np = vec3(0.0);

    // for (uint i = 0u; i < Num; i++)
    // {
    //     float h = ((float(i) + Offset) / float(Num)) * 2.0 - 1.0;

    //     Np += attenuation(p, h, Color, HairTemp) * gaussianDetector(Bp, Phi - omega(p, h, HairTemp));
    // }

    // Np *= (2.0 / float(Num));

    // return 0.5 * Np;

    float h = 0.0;
    if (p == 2)
        h = sqrt(3.0) * 0.5;

    return attenuation(p, h, Color, HairTemp) * sampleN(p, NpTex, HairTemp);
}

// [d'Eon et al. 2011, "An Energy-Conserving Hair Reflectance Model"]
// [d'Eon et al. 2014, "A Fiber Scattering Model with Non-Separable Lobes"]
vec3 evalHairRef(EpicHairBSDF bsdf, vec3 L, vec3 V, vec3 N, sampler3D NpTex, uvec2 Random) {
    float ClampedRoughness = clamp(bsdf.roughness, 1.0 / 255.0, 1.0);

    float n = bsdf.ior;

    HairTempData hairTemp;

    hairTemp.VoL       = dot(V, L);
    hairTemp.SinThetaL = dot(N, L);
    hairTemp.SinThetaV = dot(N, V);

    hairTemp.CosThetaT = sqrt(1.0 - pow((1.0 / n) * hairTemp.SinThetaL, 2.0));
    hairTemp.CosThetaD = cos(0.5 * abs(asin(hairTemp.SinThetaV) - asin(hairTemp.SinThetaL)));

    vec3 Lp = L - hairTemp.SinThetaL * N;
    vec3 Vp = V - hairTemp.SinThetaV * N;

    hairTemp.CosPhi     = dot(Lp, Vp) * inversesqrt(dot(Lp, Lp) * dot(Vp, Vp));
    hairTemp.CosHalfPhi = sqrt(0.5 + 0.5 * hairTemp.CosPhi);

    hairTemp.n_prime = sqrt(n * n - 1.0 + pow(hairTemp.CosThetaD, 2.0)) / hairTemp.CosThetaD;

    float Shift = 0.035;

    float Alpha[3] = float[3](-Shift * 2.0, Shift, Shift * 4.0);

    float B[3] = float[3](pow(ClampedRoughness, 2.0), pow(ClampedRoughness, 2.0) / 2.0, pow(ClampedRoughness, 2.0) * 2.0);

    vec3 S = vec3(0.0);

    for (uint p = 0u; p < 3u; p++)
    {
        float SinThetaV = hairTemp.SinThetaV;
        float Bp        = B[p];

        if (p == 0u)
        {
            Bp *= sqrt(2.0) * hairTemp.CosHalfPhi;

            float sa = sin(Alpha[p]);
            float ca = cos(Alpha[p]);

            SinThetaV -= 2.0 * sa * (hairTemp.CosHalfPhi * ca * sqrt(1.0 - SinThetaV * SinThetaV) + sa * SinThetaV);
        } else
        {
            SinThetaV = sin(asin(SinThetaV) - Alpha[p]);
        }

        float Mp = longitudinalScattering(Bp, hairTemp.SinThetaL, SinThetaV);

        vec3 Np = azimuthalScattering(int(p), B[p], bsdf.baseColor, hairTemp, NpTex, Random);

        vec3 Sp = Mp * Np;
        S += Sp;
    }

    return S;
}
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Hair BSDF
// Approximation to HairShadingRef using concepts from the following papers:
// [Marschner et al. 2003, "Light Scattering from Human Hair Fibers"]
// [Pekelis et al. 2015, "A Data-Driven Light Scattering Model for Hair"]
///////////////////////////////////////////////////////////////////////////////////////////////////

vec3 evalEpicHairBSDF(vec3         L,
                      vec3         V,
                      vec3         N,
                      float        shadow,
                      sampler3D    NpTex,
                      EpicHairBSDF bsdf,
                      float        inBacklit,
                      float        area,
                      bool         r,
                      bool         tt,
                      bool         trt,
                      bool         scatter) {

#if HAIR_REFERENCE
    vec3 S = evalHairRef(bsdf, L, V, N, NpTex, uvec2(0.0));
#else

    // to prevent NaN with decals
    // OR-18489 HERO: IGGY: RMB on E ability causes blinding hair effect
    // OR-17578 HERO: HAMMER: E causes blinding light on heroes with hair
    float clampedRoughness = clamp(bsdf.roughness, 1.0 / 255.0, 1.0);

    // const vec3 DiffuseN	= OctahedronToUnitVector( GBuffer.CustomData.xy * 2 - 1 );

    const float backlit = bsdf.useBacklit ? inBacklit : 1.0;
    // const float backlit =  1.0;
    // const float backlit = min(inBacklit, bsdf.useBacklit ? GBuffer.CustomData.z : 1.0);

    // THETA
    const float VoL       = dot(V, L);
    const float sinThetaL = clamp(dot(N, L), -1.0, 1.0);
    const float sinThetaV = clamp(dot(N, V), -1.0, 1.0);
    float       cosThetaD = cos(0.5 * abs(asin(sinThetaV) - asin(sinThetaL)));
    // cosThetaD reaches 0 when the strand tangent aligns with V on one side and
    // with L on the other (asin diff → π, half-cos → 0). Several terms below
    // divide by cosThetaD or feed it as a pow exponent, so an unclamped value
    // produces Inf / NaN and the BSDF either zeroes out (looks too dark) or
    // blows up to huge HDR values (renders as bright magenta after MSAA resolve
    // and tonemap). Floor it to keep the BSDF well-defined.
    cosThetaD = max(cosThetaD, 1e-3);

    float viewPerpendicularity = sqrt(max(0.0, 1.0 - sinThetaV * sinThetaV));
    float grazingTerm = viewPerpendicularity;

    // PHI
    const vec3  Lp         = L - sinThetaL * N;
    const vec3  Vp         = V - sinThetaV * N;
    const float cosPhi     = dot(Lp, Vp) * inversesqrt(dot(Lp, Lp) * dot(Vp, Vp) + 1e-4);
    const float cosHalfPhi = sqrt(saturate(0.5 + 0.5 * cosPhi));
    // const float Phi = acosFast( CosPhi );

    float n = bsdf.ior;
    // float n_prime = sqrt( n*n - 1 + Pow2( CosThetaD ) ) / CosThetaD;
    float n_prime = 1.19 / cosThetaD + 0.36 * cosThetaD;

    float shift = 0.035;
    // float shift = bsdf.shift;
    vec3 alpha = vec3(-shift * 2, shift, shift * 4);
    vec3 beta  = vec3(area + pow2(clampedRoughness), area + pow2(clampedRoughness) * 0.5, area + pow2(clampedRoughness) * 2.0);

    vec3 S = vec3(0.0);
    // R

    if (r)
    {

        const float sa        = sin(alpha[0]);
        const float ca        = cos(alpha[0]);
        float       shiftR    = 2 * sa * (ca * cosHalfPhi * sqrt(1 - sinThetaV * sinThetaV) + sa * sinThetaV);
        float       betaScale = bsdf.useSeparableR ? sqrt(2.0) * cosHalfPhi : 1.0;
        float       Mp        = g(sinThetaL + sinThetaV - shiftR, beta[0] * betaScale, bsdf.clampBSDFValue);
        float       Np        = 0.25 * cosHalfPhi;
        float       Fp        = fresnel(sqrt(saturate(0.5 + 0.5 * VoL)), n);
        S += vec3(Mp * Np * Fp * (bsdf.specular * 2.0) * mix(1, 0.0, saturate(-VoL)));
    }

    // // TT
    if (tt)
    {

        float Mp = g(sinThetaL + sinThetaV - alpha[1], beta[1], bsdf.clampBSDFValue);

        float a = 1.0 / n_prime;
        // float h = CosHalfPhi * rsqrt( 1 + a*a - 2*a * sqrt( 0.5 - 0.5 * CosPhi ) );
        // float h = CosHalfPhi * ( ( 1 - Pow2( CosHalfPhi ) ) * a + 1 );
        float h = cosHalfPhi * (1.0 + a * (0.6 - 0.8 * cosPhi));
        // float h = 0.4;
        // float yi = asinFast(h);
        // float yt = asinFast(h / n_prime);

        float f  = fresnel(cosThetaD * sqrt(saturate(1 - h * h)), n);
        float Fp = pow2(1.0 - f);
        vec3  Tp = vec3(0.0);

        if (bsdf.useLegacyAbsorption) 
        {

            Tp = pow(abs(bsdf.baseColor), vec3(0.5 * sqrt(1.0 - pow2(h * a)) / cosThetaD));

        } else
        {
            // Compute absorption color which would match user intent after multiple scattering
            const vec3 absorptionColor = hairColorToAbsorption(bsdf.baseColor);
            Tp                         = exp(-absorptionColor * 2.0 * abs(1.0 - pow2(h * a) / cosThetaD));
        }

        // float t = asin( 1 / n_prime );
        // float d = ( sqrt(2) - t ) / ( 1 - t );
        // float s = -0.5 * PI * (1 - 1 / n_prime) * log( 2*d - 1 - 2 * sqrt( d * (d - 1) ) );
        // float s = 0.35;
        // float Np = exp( (Phi - PI) / s ) / ( s * Pow2( 1 + exp( (Phi - PI) / s ) ) );
        // float Np = 0.71 * exp( -1.65 * Pow2(Phi - PI) );
        float Np = exp(-3.65 * cosPhi - 3.98);

        S += Mp * Np * Fp * Tp  * grazingTerm;
    }

    // TRT
    if (trt)
    {

        float Mp = g(sinThetaL + sinThetaV - alpha[2], beta[2], bsdf.clampBSDFValue);

        // float h = 0.75;
        float f  = fresnel(cosThetaD * 0.5, n);
        float Fp = pow2(1.0 - f) * f;
        // vec3 Tp = pow( GBuffer.BaseColor, 1.6 / CosThetaD );
        vec3 Tp = pow(abs(bsdf.baseColor), vec3(0.8 / cosThetaD));

        // float s = 0.15;
        // float Np = 0.75 * exp( Phi / s ) / ( s * Pow2( 1 + exp( Phi / s ) ) );
        float Np = exp(17.0 * cosPhi - 16.78);

        S += Mp * Np * Fp * Tp * grazingTerm;
        
    }
#endif

    if (scatter)
    {
        S = bsdf.globalScattering * (S + bsdf.localScattering) * bsdf.opaqueVisibility;
        // Fallback ...
        S += evalKajiyaKayDiffuseAttenuation(bsdf.baseColor, bsdf.metallic, L, V, N, 1.0 - bsdf.opaqueVisibility);
    }

    S = -min(-S, 0.0);

    return S ;
}

// ── Eyelash variant ─────────────────────────────────────────────────────────
// Same Marschner R/TT/TRT structure as evalEpicHairBSDF, but with the three knobs
// the epic path leaves inert actually wired in, so eyelashes can be tuned to be
// far less reflective and far more transmissive than scalp hair:
//   • R   (surface sheen)   scaled by Rpower  — dial the plastic-wire glare down.
//   • TT  (transmission)    scaled by TTpower and modulated by backlit — real
//                           light-through-the-fiber glow, strongest when backlit.
//   • TRT (secondary glint) scaled by TRTpower — tame the coloured highlight.
// Only eyelash_strand.glsl calls this; scalp hair/eyebrows keep evalEpicHairBSDF,
// so wiring these multipliers here cannot change the existing hair look.
vec3 evalEyelashBSDF(vec3         L,
                     vec3         V,
                     vec3         N,
                     float        shadow,
                     sampler3D    NpTex,
                     EpicHairBSDF bsdf,
                     float        inBacklit,
                     float        area,
                     bool         r,
                     bool         tt,
                     bool         trt,
                     bool         scatter) {

    float clampedRoughness = clamp(bsdf.roughness, 1.0 / 255.0, 1.0);

    const float backlit = bsdf.useBacklit ? inBacklit : 1.0;

    // THETA
    const float VoL       = dot(V, L);
    const float sinThetaL = clamp(dot(N, L), -1.0, 1.0);
    const float sinThetaV = clamp(dot(N, V), -1.0, 1.0);
    float       cosThetaD = cos(0.5 * abs(asin(sinThetaV) - asin(sinThetaL)));
    cosThetaD             = max(cosThetaD, 1e-3); // see evalEpicHairBSDF for why

    float viewPerpendicularity = sqrt(max(0.0, 1.0 - sinThetaV * sinThetaV));
    float grazingTerm          = viewPerpendicularity;

    // PHI
    const vec3  Lp         = L - sinThetaL * N;
    const vec3  Vp         = V - sinThetaV * N;
    const float cosPhi     = dot(Lp, Vp) * inversesqrt(dot(Lp, Lp) * dot(Vp, Vp) + 1e-4);
    const float cosHalfPhi = sqrt(saturate(0.5 + 0.5 * cosPhi));

    float n       = bsdf.ior;
    float n_prime = 1.19 / cosThetaD + 0.36 * cosThetaD;

    float shift = 0.035;
    vec3  alpha = vec3(-shift * 2, shift, shift * 4);
    vec3  beta  = vec3(area + pow2(clampedRoughness), area + pow2(clampedRoughness) * 0.5, area + pow2(clampedRoughness) * 2.0);

    vec3 S = vec3(0.0);

    // R — primary surface sheen (Rpower now active: lower it to kill the glare)
    if (r)
    {
        const float sa        = sin(alpha[0]);
        const float ca        = cos(alpha[0]);
        float       shiftR    = 2 * sa * (ca * cosHalfPhi * sqrt(1 - sinThetaV * sinThetaV) + sa * sinThetaV);
        float       betaScale = bsdf.useSeparableR ? sqrt(2.0) * cosHalfPhi : 1.0;
        float       Mp        = g(sinThetaL + sinThetaV - shiftR, beta[0] * betaScale, bsdf.clampBSDFValue);
        float       Np        = 0.25 * cosHalfPhi;
        float       Fp        = fresnel(sqrt(saturate(0.5 + 0.5 * VoL)), n);
        S += vec3(Mp * Np * Fp * (bsdf.specular * 2.0) * bsdf.Rpower * mix(1, 0.0, saturate(-VoL)));
    }

    // TT — transmission (TTpower + backlit now active: the translucency knob)
    if (tt)
    {
        float Mp = g(sinThetaL + sinThetaV - alpha[1], beta[1], bsdf.clampBSDFValue);

        float a = 1.0 / n_prime;
        float h = cosHalfPhi * (1.0 + a * (0.6 - 0.8 * cosPhi));

        float f  = fresnel(cosThetaD * sqrt(saturate(1 - h * h)), n);
        float Fp = pow2(1.0 - f);
        vec3  Tp = vec3(0.0);

        if (bsdf.useLegacyAbsorption)
        {
            Tp = pow(abs(bsdf.baseColor), vec3(0.5 * sqrt(1.0 - pow2(h * a)) / cosThetaD));
        } else
        {
            const vec3 absorptionColor = hairColorToAbsorption(bsdf.baseColor);
            Tp                         = exp(-absorptionColor * 2.0 * abs(1.0 - pow2(h * a) / cosThetaD));
        }

        float Np = exp(-3.65 * cosPhi - 3.98);

        S += Mp * Np * Fp * Tp * grazingTerm * bsdf.TTpower * backlit;
    }

    // TRT — secondary glint (TRTpower now active: tame the coloured highlight)
    if (trt)
    {
        float Mp = g(sinThetaL + sinThetaV - alpha[2], beta[2], bsdf.clampBSDFValue);

        float f  = fresnel(cosThetaD * 0.5, n);
        float Fp = pow2(1.0 - f) * f;
        vec3  Tp = pow(abs(bsdf.baseColor), vec3(0.8 / cosThetaD));

        float Np = exp(17.0 * cosPhi - 16.78);

        S += Mp * Np * Fp * Tp * grazingTerm * bsdf.TRTpower;
    }

    if (scatter)
    {
        S = bsdf.globalScattering * (S + bsdf.localScattering) * bsdf.opaqueVisibility;
        S += evalKajiyaKayDiffuseAttenuation(bsdf.baseColor, bsdf.metallic, L, V, N, 1.0 - bsdf.opaqueVisibility);
    }

    S = -min(-S, 0.0);

    return S;
}

// Dual scattering computation are done here for faster iteration (i.e., does not invalidate tons of shaders)
EpicHairBSDF computeDualScatteringTerms(const HairTransmittanceMask TransmittanceMask,
                                        const HairAverageScattering AverageScattering,
                                        const vec3                  V,
                                        const vec3                  L,
                                        const vec3                  T,
                                        EpicHairBSDF                bsdf) {
    const float SinThetaL                 = clamp(dot(T, L), -1.0, 1.0);
    const float SinThetaV                 = clamp(dot(T, V), -1.0, 1.0);
    const float CosThetaL                 = sqrt(1.0 - SinThetaL * SinThetaL);
    const float maxAverageScatteringValue = 0.99;

    // Straight implementation of the dual scattering paper
    const vec3 af          = min(vec3(maxAverageScatteringValue), AverageScattering.A_front);
    const vec3 af2         = pow2(af);
    const vec3 ab          = min(vec3(maxAverageScatteringValue), AverageScattering.A_back);
    const vec3 ab2         = pow2(ab);
    const vec3 OneMinusAf2 = 1.0 - af2;

    const vec3 A1 = ab * af2 / OneMinusAf2;
    const vec3 A3 = ab * ab2 * af2 / (OneMinusAf2 * pow2(OneMinusAf2));
    const vec3 Ab = A1 + A3;

    // Add a min/max roughness for dual scattering based. This is a bit adhoc, but
    // * Min/lower bound helps with BSDF being too narrow and causing some fireflies,
    // * Max/upper bound helps against "too-flat" look due to dual scattering assuming directional lobe (vs. more radially uniform)
    float       roughness = clamp(bsdf.roughness, 0.18, 0.6);
    const float Beta_R    = pow2(roughness);
    const float Beta_TT   = pow2(roughness * 0.5);
    const float Beta_TRT  = pow2(roughness * 2);

    const float Shift = 0.035;
    // const float Shift = bsdf.shift;
    const float Shift_R   = -0.035 * 2.0;
    const float Shift_TT  = 0.035;
    const float Shift_TRT = 0.035 * 4.0;

    // Average density factor (This is the constant used in the original paper)
    const float df = 0.7;
    const float db = 0.7;

    // Always shift the hair count by one to remove self-occlusion/shadow aliasing and have smoother transition
    // This insure the the pow function always starts at 0 for front facing hair
    const float HairCount = max(0.0, float(TransmittanceMask.hairCount) - 1.0);

    // This is a coarse approximation of eq. 13. Normally, Beta_f should be weighted by the 'normalized'
    // R, TT, and TRT terms.
    // Protect against af being all-zero (LUT can return 0 at corners) — division would be NaN
    // and propagate through Beta_f → sigma_f2 → Sf, eventually surfacing as bright magenta.
    const float afSum = max(af.r + af.g + af.b, 1e-5);
    const vec3 af_weights = af / afSum;
    const vec3 Beta_f     = vec3(dot(vec3(Beta_R, Beta_TT, Beta_TRT), af_weights));
    const vec3 Beta_f2    = Beta_f * Beta_f;
    const vec3 sigma_f2   = Beta_f2 * max(1.0, HairCount);

    const float Theta_d = asin(SinThetaL) + asin(SinThetaV);
    const float Theta_h = Theta_d * 0.5;

    // Global scattering spread 'Sf'
    vec3       Sf = vec3(g2(Theta_h, sigma_f2.r), g2(Theta_h, sigma_f2.g), g2(Theta_h, sigma_f2.b)) / PI;
    const vec3 Tf = pow(AverageScattering.A_front, vec3(HairCount));

    // Overall shift due to the various local scatteing event (all shift & roughnesss should vary with color)
    const vec3 shift_f = vec3(dot(vec3(Shift_R, Shift_TT, Shift_TRT), af_weights));
    const vec3 shift_b = shift_f;
    const vec3 delta_b = shift_b * (1 - 2 * ab2 / pow2(1 - af2)) * shift_f * (2 * pow2(1 - af2) + 4 * af2 * ab2) / ((1 - af2) * (1 - af2) * (1 - af2));

    // Same protection as af_weights — ab can be all-zero at LUT corners.
    const float abSum = max(ab.r + ab.g + ab.b, 1e-5);
    const vec3 ab_weights = ab / abSum;
    const vec3 Beta_b     = vec3(dot(vec3(Beta_R, Beta_TT, Beta_TRT), ab_weights));
    const vec3 Beta_b2    = Beta_b * Beta_b;

    // sigma_b's denominator can also be zero when ab is zero; clamp to keep math finite.
    const vec3 sigmaBDenom = max(ab + ab * ab2 * (2 * Beta_f + 3 * Beta_b), vec3(1e-5));
    const vec3 sigma_b =
        (1 + db * af2) * (ab * sqrt(2 * Beta_f2 + Beta_b2) + ab * ab2 * sqrt(2 * Beta_f2 + Beta_b2)) / sigmaBDenom;
    const vec3 sigma_b2 = sigma_b * sigma_b;

    // Local scattering Spread 'Sb'
    // In Efficient Implementation of the Dual Scattering Model, the variance for back scattering is the sum of the front & back variances
    vec3 Sb = vec3(g2(Theta_h - delta_b.r, sigma_f2.r + sigma_b2.r),
                   g2(Theta_h - delta_b.g, sigma_f2.g + sigma_b2.g),
                   g2(Theta_h - delta_b.b, sigma_f2.b + sigma_b2.b)) /
              PI;

    const vec3 LocalScattering = 2.0 * Ab * Sb * db;

    // Different variant for managing sefl-occlusion issue for global scattering
    const vec3 GlobalScattering = mix(vec3(1.0), Tf * Sf * df, saturate(HairCount));
    // const vec3 GlobalScattering = clamp(Tf - vec3(TransmittanceMask.visibility),0.0,1.0) * df * ( Sf  + LocalScattering);
    // const vec3 GlobalScattering =  clamp(Tf - vec3(TransmittanceMask.visibility),0.0,1.0) * df * Sf;

    bsdf.globalScattering = GlobalScattering;
    bsdf.localScattering  = LocalScattering;
    bsdf.opaqueVisibility = TransmittanceMask.visibility;
    return bsdf;
}

HairAverageScattering sampleHairLUT(sampler3D LUTTexture, vec3 InAbsorption, float Roughness, float SinViewAngle) {
    const vec3 RemappedAbsorption = fromLinearAbsorption(InAbsorption);
    
    const vec2 LUTValue_R = textureLod(LUTTexture, vec3(saturate(abs(SinViewAngle)), saturate(Roughness), saturate(RemappedAbsorption.x)), 0.0).xy;
    const vec2 LUTValue_G = textureLod(LUTTexture, vec3(saturate(abs(SinViewAngle)), saturate(Roughness), saturate(RemappedAbsorption.y)), 0.0).xy;
    const vec2 LUTValue_B = textureLod(LUTTexture, vec3(saturate(abs(SinViewAngle)), saturate(Roughness), saturate(RemappedAbsorption.z)), 0.0).xy;

    HairAverageScattering Output;
    Output.A_front = vec3(LUTValue_R.x, LUTValue_G.x, LUTValue_B.x);
    Output.A_back  = vec3(LUTValue_R.y, LUTValue_G.y, LUTValue_B.y);
    return Output;
}

EpicHairBSDF
evalHairMultipleScattering(const vec3 V, const vec3 L, const vec3 T, HairTransmittanceMask TransmittanceMask, sampler3D HairLUTTexture, EpicHairBSDF bsdf) {
    // Hack: Override the actual roughness, with a different value to achieve a specific look
    // const float Roughness = GBuffer.CustomData.x > 0 ? GBuffer.CustomData.x : GBuffer.Roughness;
    // const float Backlit = GBuffer.CustomData.z;

    // Compute the transmittance based on precompute Hair transmittance LUT

    const float                 SinLightAngle     = dot(L, T);
    const HairAverageScattering AverageScattering = sampleHairLUT(HairLUTTexture, bsdf.baseColor, bsdf.roughness, SinLightAngle);
    // const HairAverageScattering AverageScattering;

    return computeDualScatteringTerms(TransmittanceMask, AverageScattering, V, L, T, bsdf);
}

// Transmittance (Marshnerr Paper)
vec3 T(vec3 sigma_a, float gammaT) {
    return exp(-2.0 * sigma_a * (1.0 + cos(2.0 * gammaT)));
}
// Transmittance (Beer-Lambert Law)
vec3 T(vec3 sigma_a, float cosGammaT, float cosThetaT) {
    return exp(-sigma_a * (2.0 * cosGammaT / cosThetaT));
}

// Attenuation
vec3 Ap(int p, float h, float ior, float iorPerp, float cosThetaD, vec3 sigma_a, float cosThetaT) {
    // Aproximation by Epic/Frostbyte/Unity
    float f = fresnel(ior, cosThetaD * sqrt(1.0 - (h * h)));

#ifdef TRANS_MARSHNER
    // Actual Bravais Index
    // float iorPerp = bravaisIndex(thD, ior);
    float gammaT = asin(h / iorPerp);

    // Transmittance
    vec3 t = T(sigma_a, gammaT);
#endif
#ifdef TRANS_PHARR
    // Actual Bravais Index
    // float iorPerp = bravaisIndex(thD, ior);
    float gammaT = asin(h / iorPerp);

    // Transmittance
    vec3 t = T(sigma_a, cos(gammaT), cosThetaT);
#endif
#ifdef TRANS_EPIC
    // float iorPerp = bravaisIndex(thD, ior);
    float a = 1.0 / iorPerp;

    // Transmittance
    vec3 t = vec3(0.0);
    if (p == 1)
    {
        float exponent = sqrt(1.0 - h * h * a * a) / (2.0 * cosThetaD);
        t              = pow(sigma_a, vec3(exponent));
    }
    if (p == 2)
    {
        float exponent = 0.8 / cosThetaD;
        t              = pow(sigma_a, vec3(exponent));
    }

#endif

    return ((1 - f) * (1 - f)) * pow(f, p - 1) * pow(t, vec3(p));
}

vec3 evalHairBSDF(vec3         L,
                  vec3         V,
                  vec3         N,
                  float        shadow,
                  sampler3D    NpTex,
                  EpicHairBSDF bsdf,
                  float        inBacklit,
                  float        area,
                  bool         r,
                  bool         tt,
                  bool         trt,
                  bool         scatter) {

#if HAIR_REFERENCE
    vec3 S = evalHairRef(bsdf, L, V, N, NpTex, uvec2(0.5, 0.5));
#else

    vec3 bsdfSigma = bsdf.baseColor; // Warp

    // to prevent NaN with decals
    // OR-18489 HERO: IGGY: RMB on E ability causes blinding hair effect
    // OR-17578 HERO: HAMMER: E causes blinding light on heroes with hair
    float clampedRoughness = clamp(bsdf.roughness, 1.0 / 255.0, 1.0);

    const float backlit = bsdf.useBacklit ? inBacklit : 1.0;
    // const float backlit =  1.0;
    // const float backlit = min(inBacklit, bsdf.useBacklit ? GBuffer.CustomData.z : 1.0);

    // THETA
    const float VoL       = dot(V, L);
    const float sinThetaL = clamp(dot(N, L), -1.0, 1.0);
    const float sinThetaV = clamp(dot(N, V), -1.0, 1.0);
    float       cosThetaD = cos(0.5 * abs(asin(sinThetaV) - asin(sinThetaL)));
    // See evalEpicHairBSDF — same instability at degenerate strand/V/L alignment.
    cosThetaD = max(cosThetaD, 1e-3);

    // PHI
    const vec3  Lp         = L - sinThetaL * N;
    const vec3  Vp         = V - sinThetaV * N;
    const float cosPhi     = dot(Lp, Vp) * inversesqrt(dot(Lp, Lp) * dot(Vp, Vp) + 1e-4);
    const float cosHalfPhi = sqrt(saturate(0.5 + 0.5 * cosPhi));
    // const float Phi = acosFast( CosPhi );

    float n         = bsdf.ior;
    float sinThetaT = sinThetaL / n;
    float cosThetaT = sqrt(1.0 - sinThetaT * sinThetaT);
    float n_prime   = 1.19 / cosThetaD + 0.36 * cosThetaD;

    float shift = 0.035;
    // float shift = bsdf.shift;
    vec3 alpha = vec3(-shift * 2, shift, shift * 4);
    vec3 beta  = vec3(area + pow2(clampedRoughness), area + pow2(clampedRoughness) * 0.5, area + pow2(clampedRoughness) * 2.0);

    vec3 S = vec3(0.0);
    // R

    if (r)
    {

        const float sa        = sin(alpha[0]);
        const float ca        = cos(alpha[0]);
        float       shiftR    = 2 * sa * (ca * cosHalfPhi * sqrt(1 - sinThetaV * sinThetaV) + sa * sinThetaV);
        float       betaScale = bsdf.useSeparableR ? sqrt(2.0) * cosHalfPhi : 1.0;
        float       Mp        = g(sinThetaL + sinThetaV - shiftR, beta[0] * betaScale, bsdf.clampBSDFValue);
        float       Np        = 0.25 * cosHalfPhi;
        float       Fp        = fresnel(sqrt(saturate(0.5 + 0.5 * VoL)), n);
        S += vec3(Mp * Np * Fp * (bsdf.specular * 2.0) * mix(1, backlit, saturate(-VoL)));
    }

    // // TT
    if (tt)
    {

        float Mp = g(sinThetaL + sinThetaV - alpha[1], beta[1], bsdf.clampBSDFValue);

        float a = 1.0 / n_prime;
        float h = cosHalfPhi * (1.0 + a * (0.6 - 0.8 * cosPhi));

        // float f  = fresnel(cosThetaD * sqrt(saturate(1 - h * h)), n);
        vec3  Ap = Ap(1, h, n, n_prime, cosThetaD, bsdfSigma, cosThetaT);
        float Np = exp(-3.65 * cosPhi - 3.98);

        S += Mp * Np * Ap * backlit;
    }

    // TRT
    if (trt)
    {

        float Mp = g(sinThetaL + sinThetaV - alpha[2], beta[2], bsdf.clampBSDFValue);

        float h = sqrt(3.0) * 0.5;
        // float f  = fresnel(cosThetaD * 0.5, n);
        // float Fp = pow2(1.0 - f) * f;
        // // vec3 Tp = pow( GBuffer.BaseColor, 1.6 / CosThetaD );
        // vec3 Tp = pow(abs(bsdf.baseColor), vec3(0.8 / cosThetaD));

        // float s = 0.15;
        // float Np = 0.75 * exp( Phi / s ) / ( s * Pow2( 1 + exp( Phi / s ) ) );
        vec3  Ap = Ap(2, h, n, n_prime, cosThetaD, bsdfSigma, cosThetaT);
        float Np = exp(17.0 * cosPhi - 16.78);

        S += Mp * Np * Ap;
    }

#endif
    if (scatter)
    {
        S = bsdf.globalScattering * (S + bsdf.localScattering) * bsdf.opaqueVisibility;
        // S = (S + bsdf.localScattering) * bsdf.opaqueVisibility;
        //  S = bsdf.globalScattering;
        // S = bsdf.localScattering;
        // Fallback ...
        S += evalKajiyaKayDiffuseAttenuation(bsdf.baseColor, bsdf.metallic, L, V, N, 1.0 - bsdf.opaqueVisibility);
    }

    S = -min(-S, 0.0);

    return S;
}
