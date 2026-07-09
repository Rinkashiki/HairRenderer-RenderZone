// eye.glsl — Contains:
// 1) Procedural eyeball socket occlusion with a warm sclera shadow tint.
// 2) Parallax occlusion mapping for cornea refraction and iris effect.

// Overview: Procedural eyeball socket occlusion:
//
// Approximates the soft darkening where the eyelids contact the sclera, so the
// eyeball reads as sitting INSIDE the socket instead of looking like a glued-on
// ball. This is the "typical eye occlusion" — a smooth gradient, darker in the
// corners and (more so) under the upper lid.
//
// Rather than darkening toward neutral black, the occlusion is pushed through a
// small color ramp: occluded sclera in reality is WARM (creamy pale-yellow at
// the lid contact, shifting reddish at the deepest corners from vascularization
// / shallow subsurface), so a black AO on a wet white ball looks grimy. The
// ramp is procedural (no texture / no binding) — a LUT in everything but storage.
//
// Anchor: the eyeball's OBJECT-SPACE surface normal. Because the eyeball is a
// sphere, that normal is the direction on the ball, so dot(normal, irisAxis)
// tells us cornea (~1) vs. lid-contact rim (toward 0/negative). Working in
// object space keeps the effect independent of the mesh's world placement /
// rotation and stable as the camera moves. Valid while the eyes are STATIC
// (no gaze) — if gaze is ever added, this must be re-anchored to the socket
// rather than the (then rotating) eyeball.
//
// All tuning lives here as consts. HairViewer recompiles shaders on relaunch,
// so just edit and restart — no rebuild, no GUI.

// Overview: Parallax occlusion mapping:
// Simulate refraction of light passing through the cornea with parallax occlusion mapping.


// Object-space direction the cornea/iris faces. Calibrated against the Maria/Nadia
// rig via the grayscale occN debug: the cornea normal points along +Z, so the
// iris stays clean (occN~0) and occlusion grows toward the lid rim. Flip to
// vec3(0,0,-1) if a different rig inverts (whole eyeball occluded).
const vec3  EYE_IRIS_AXIS   = vec3(0.0, 0.0, 1.0);
// Object-space "up" (toward the upper lid).
const vec3  EYE_UP_AXIS     = vec3(0.0, 1.0, 0.0);

// dot(normal, irisAxis) range over which occlusion ramps in:
//   >= EYE_AO_CORNEA -> fully lit (clear cornea / iris)
//   <= EYE_AO_RIM    -> maximum lid occlusion
const float EYE_AO_CORNEA   = 1.0; // clean zone = only the iris-facing cap (higher = smaller)
const float EYE_AO_RIM      = 0.30; // full occlusion well within the visible sclera

const float EYE_AO_STRENGTH = 1.0; // overall intensity of the warm shadow tint (0..1)
const float EYE_UPPER_BIAS  = 0.28; // extra darkening toward the upper lid

// Minimum ambient on the sclera, as a fraction of its own albedo, so an eye on
// the shadowed side of the face never crushes to black (a glossy eyeball with no
// catchlight otherwise reads as a dead hole). 0 = off. Pupil/iris stay dark since
// their albedo is dark; only the bright sclera gets meaningfully lifted.
const float EYE_AMBIENT_FLOOR = 0.15;

// Sclera shadow ramp. Authored in sRGB, pre-converted to the renderer's LINEAR
// space (these are multiplied into linear HDR color). Open sclera = no tint,
// pale yellow at the lid contact, warm red-brown at the deepest corners.
const vec3  EYE_TINT_OPEN   = vec3(1.0,   1.0,    1.0  ); // sRGB #FFFFFF — no shadow
const vec3  EYE_TINT_MID    = vec3(0.888, 0.815,  0.577); // sRGB #F2E9C8 — pale yellow
const vec3  EYE_TINT_DEEP   = vec3(0.254, 0.0685, 0.042); // sRGB #8A4A3A — warm red-brown

// Occlusion value at which the ramp reaches EYE_TINT_DEEP. The visible sclera
// rarely hits occN=1 (the deepest contact hides behind the lid), so without
// this remap the corners never get past the yellow midpoint. Lower = redder sooner.
const float EYE_RAMP_PEAK   = 0.6;

// 3-stop ramp: t in [0,1] -> shadow color (open -> mid -> deep).
vec3 sclera_ramp(float t) {
    return (t < 0.5)
        ? mix(EYE_TINT_OPEN, EYE_TINT_MID,  t * 2.0)
        : mix(EYE_TINT_MID,  EYE_TINT_DEEP, (t - 0.5) * 2.0);
}

// Geometric occlusion amount in [0,1]: 0 at the cornea, 1 at the lid-contact rim
// (upper lid deepest). Exposed so debug views can read the falloff shape.
float eye_occ_amount(vec3 objNormal) {
    vec3 n = normalize(objNormal);
    float rim = 1.0 - smoothstep(EYE_AO_RIM, EYE_AO_CORNEA, dot(n, normalize(EYE_IRIS_AXIS)));
    float upper = max(dot(n, normalize(EYE_UP_AXIS)), 0.0);
    return clamp(rim * (1.0 + EYE_UPPER_BIAS * upper), 0.0, 1.0);
}

// Returns a linear-space color multiplier: white (1,1,1) at the cornea, warm and
// darker toward the lid contact (upper lid deepest).
vec3 eye_occlusion(vec3 objNormal) {
    // Remap occlusion so the ramp hits DEEP at EYE_RAMP_PEAK (see note above),
    // then scale by overall strength (white = unaffected).
    float t = clamp(eye_occ_amount(objNormal) / EYE_RAMP_PEAK, 0.0, 1.0);
    return mix(vec3(1.0), sclera_ramp(t), EYE_AO_STRENGTH);
}

// Full eye shading hook for the masked eyeball fragment. Applies the warm
// occlusion tint to the lit color, then floors against the eye's own albedo so
// the sclera keeps a little ambient in shadow instead of going black.
//   color     : lit linear color so far
//   albedo    : the eye's base color (sclera bright, iris/pupil dark)
//   objNormal : object-space surface normal
//   eyeAmount : eye-mask coverage [0,1] (fades both effects at the mask edge)
vec3 eye_shade(vec3 color, vec3 albedo, vec3 objNormal, float eyeAmount) {
    color *= mix(vec3(1.0), eye_occlusion(objNormal), eyeAmount);
    color  = max(color, albedo * (EYE_AMBIENT_FLOOR * eyeAmount));
    return color;
}

// Eye parallax logic. Given a point on the eyeball and the view direction, return the
// new UVs, which correspond to the iris texel we're supposed to be seeing. Simulates
// the effect of light refraction on the cornea.
// NOTE : Implements Parallax Occlusion Mapping.
vec2 eye_parallax_uv(vec2 uv, vec3 viewTS) {
	// Basic temporary implementation for testing.
	float irisDepth = 0.04;
	vec2 newUV = uv - viewTS.xy / max(viewTS.z, 0.2) * irisDepth;
	return newUV;
}
