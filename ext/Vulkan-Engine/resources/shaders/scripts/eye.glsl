// eye.glsl — procedural eyeball socket occlusion (no textures, no GUI).
//
// Approximates the soft darkening where the eyelids contact the sclera, so the
// eyeball reads as sitting INSIDE the socket instead of looking like a glued-on
// ball. This is the "typical eye occlusion" — a smooth gradient, darker in the
// corners and (more so) under the upper lid.
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

// Object-space direction the cornea/iris faces. Verified against the Maria/Nadia
// rig: the cornea normal points along -Z, so the iris stays lit and the lid
// contact darkens. Flip to vec3(0,0,1) if a different rig darkens the wrong side.
const vec3  EYE_IRIS_AXIS   = vec3(0.0, 0.0, -1.0);
// Object-space "up" (toward the upper lid).
const vec3  EYE_UP_AXIS     = vec3(0.0, 1.0, 0.0);

// dot(normal, irisAxis) range over which occlusion ramps in:
//   >= EYE_AO_CORNEA -> fully lit (clear cornea / iris)
//   <= EYE_AO_RIM    -> maximum lid occlusion
const float EYE_AO_CORNEA   = 0.60;
const float EYE_AO_RIM      = -0.20;

const float EYE_AO_STRENGTH = 0.38; // how dark the lid contact gets (0..1)
const float EYE_UPPER_BIAS  = 0.28; // extra darkening toward the upper lid

// Returns a [0,1] multiplier: 1 = unoccluded (cornea), < 1 = lid-shadowed.
float eye_occlusion(vec3 objNormal) {
    vec3 n = normalize(objNormal);

    // 0 at the cornea, 1 out at the lid-contact rim.
    float rim = 1.0 - smoothstep(EYE_AO_RIM, EYE_AO_CORNEA, dot(n, normalize(EYE_IRIS_AXIS)));

    // The upper lid casts a deeper contact shadow than the lower lid.
    float upper = max(dot(n, normalize(EYE_UP_AXIS)), 0.0);

    float occ = rim * (EYE_AO_STRENGTH + EYE_UPPER_BIAS * upper);
    return clamp(1.0 - occ, 0.0, 1.0);
}
