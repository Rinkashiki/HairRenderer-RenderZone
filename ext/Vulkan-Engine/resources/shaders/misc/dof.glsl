#shader vertex
#version 460

layout(location = 0) in vec3 pos;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec2 v_uv;

void main() {
    gl_Position = vec4(pos, 1.0);
    v_uv = uv;
}

#shader fragment
#version 460

layout(location = 0) in vec2 v_uv;

// Binding 0: depth   (raw gl_FragCoord.z stored in R, perspectiveRH_ZO)
layout(set = 0, binding = 0) uniform sampler2D depthTex;
// Binding 1: HDR scene color (from bloom pass)
layout(set = 0, binding = 1) uniform sampler2D colorTex;
// Binding 2: DoF parameters
layout(set = 0, binding = 2) uniform DoFBlock {
    mat4  invProjection;
    vec2  screenSize;
    float focusDistance;
    float focusRange;
    float nearBlurScale;
    float farBlurScale;
    float maxCoC;
    int   enabled;
} dof;

layout(location = 0) out vec4 outColor;

// Number of bokeh taps on the gather disk. Golden-angle spiral gives an even,
// round distribution without a precomputed kernel.
const int   TAPS   = 48;
const float GOLDEN = 2.39996323; // radians (137.5 degrees)

// Reconstruct positive view-space distance (along -Z) from an NDC depth value.
// perspectiveRH_ZO: camera looks down -Z, visible geometry has viewPos.z < 0.
float viewDistance(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = dof.invProjection * clip;
    return -(view.z / view.w);
}

// Circle-of-confusion radius (pixels) for a given view-space distance.
// Sharp inside [focusDistance - focusRange, focusDistance + focusRange];
// grows linearly outside it at the near/far rates, capped at maxCoC.
float circleOfConfusion(float dist) {
    float nearAmt = max((dof.focusDistance - dof.focusRange) - dist, 0.0);
    float farAmt  = max(dist - (dof.focusDistance + dof.focusRange), 0.0);
    float coc     = nearAmt * dof.nearBlurScale + farAmt * dof.farBlurScale;
    return clamp(coc, 0.0, dof.maxCoC);
}

void main() {
    vec3 centerColor = texture(colorTex, v_uv).rgb;

    float centerCoC = circleOfConfusion(viewDistance(v_uv, texture(depthTex, v_uv).r));

    // Disabled, or in-focus pixel: cheap passthrough so tonemapping reads valid color.
    if (dof.enabled == 0 || centerCoC < 0.5) {
        outColor = vec4(centerColor, 1.0);
        return;
    }

    vec3  sum  = centerColor;
    float wsum = 1.0;

    for (int i = 0; i < TAPS; ++i) {
        float t   = (float(i) + 0.5) / float(TAPS);
        float r   = sqrt(t);                 // uniform-area disk
        float a   = float(i) * GOLDEN;
        vec2  dir = vec2(cos(a), sin(a)) * r;

        vec2 offsetPx = dir * centerCoC;     // gather radius scaled by this pixel's CoC
        vec2 sampleUV = v_uv + offsetPx / dof.screenSize;

        vec3  c          = texture(colorTex, sampleUV).rgb;
        float sampleCoC  = circleOfConfusion(viewDistance(sampleUV, texture(depthTex, sampleUV).r));

        // Scatter-as-gather weight: a sharp (in-focus) sample must NOT bleed into a
        // blurred neighbour. Only let a sample contribute if its own CoC reaches at
        // least as far as its distance from the center.
        float sampleDistPx = length(offsetPx);
        float w            = clamp(sampleCoC - sampleDistPx + 1.0, 0.0, 1.0);

        sum  += c * w;
        wsum += w;
    }

    outColor = vec4(sum / wsum, 1.0);
}
