#shader vertex
#version 460
// Selection outline composite — see OutlineCompositePass.

layout(location = 0) in vec3 pos;

void main() {
    gl_Position = vec4(pos, 1.0);
}

#shader fragment
#version 460

layout(set = 0, binding = 0) uniform sampler2D inputImage;  // tonemapped scene
layout(set = 0, binding = 1) uniform sampler2D outlineMask; // R = covered, G = visible

layout(push_constant) uniform OutlineParams {
    vec4  color;       // linear RGB + opacity
    float width;       // half-width in pixels
    float hiddenAlpha; // opacity multiplier where occluded
    int   enabled;     // 0 = passthrough
    int   _pad;
} params;

layout(location = 0) out vec4 outputImage;

void main() {
    ivec2 p   = ivec2(gl_FragCoord.xy);
    vec4  src = texelFetch(inputImage, p, 0);
    outputImage = src;
    if (params.enabled == 0)
        return;

    // Inside the object: untouched.
    if (texelFetch(outlineMask, p, 0).r > 0.5)
        return;

    // Distance to the nearest covered texel within the ring, plus whether any
    // covered texel in reach is visible.
    ivec2 size    = textureSize(outlineMask, 0);
    int   r       = int(ceil(params.width + 0.5));
    float best    = 1e9;
    float visible = 0.0;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) {
            ivec2 q = p + ivec2(x, y);
            if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, size)))
                continue;
            vec2 m = texelFetch(outlineMask, q, 0).rg;
            if (m.r < 0.5)
                continue;
            float d = length(vec2(x, y));
            best    = min(best, d);
            if (d <= params.width + 0.5)
                visible = max(visible, m.g);
        }

    // One-pixel soft edge on the outside of the ring.
    float a = clamp(params.width + 0.5 - best, 0.0, 1.0);
    a *= mix(params.hiddenAlpha, 1.0, visible) * params.color.a;
    outputImage = vec4(mix(src.rgb, params.color.rgb, a), src.a);
}
