#shader vertex
#version 460
// Selection outline mask — strand hair (LINE_LIST). Each segment is widened to a
// screen-space quad so a groom masks as one solid shape; the composite then
// outlines its envelope instead of every individual fiber. See OutlineMaskPass.

layout(location = 0) in vec3 pos;

void main() {
    gl_Position = vec4(pos, 1.0);
}

#shader geometry
#version 460
#include camera.glsl
#include object.glsl

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

// Half-width of the widened fiber in pixels. Large enough to close the gaps
// between neighbouring strands of a groom, small enough to keep its silhouette.
const float HALF_WIDTH_PX = 2.5;

void main() {
    vec4 c0 = camera.viewProj * object.model * gl_in[0].gl_Position;
    vec4 c1 = camera.viewProj * object.model * gl_in[1].gl_Position;
    if (c0.w <= 1e-4 || c1.w <= 1e-4)
        return; // segment crosses the camera plane — skip rather than explode

    vec2 px   = camera.screenExtent;
    vec2 s0   = c0.xy / c0.w;
    vec2 s1   = c1.xy / c1.w;
    vec2 d    = (s1 - s0) * px;
    float len = length(d);
    vec2 dir  = len > 1e-5 ? d / len : vec2(1.0, 0.0);
    // Offsets in NDC (1 px = 2 / extent). Extending along the segment too
    // closes the joints between consecutive segments.
    vec2 n = vec2(-dir.y, dir.x) * HALF_WIDTH_PX * 2.0 / px;
    vec2 e = dir * HALF_WIDTH_PX * 2.0 / px;

    gl_Position = vec4((s0 - e + n) * c0.w, c0.z, c0.w); EmitVertex();
    gl_Position = vec4((s0 - e - n) * c0.w, c0.z, c0.w); EmitVertex();
    gl_Position = vec4((s1 + e + n) * c1.w, c1.z, c1.w); EmitVertex();
    gl_Position = vec4((s1 + e - n) * c1.w, c1.z, c1.w); EmitVertex();
    EndPrimitive();
}

#shader fragment
#version 460
#include camera.glsl

layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(location = 0) out vec4 outMask;

void main() {
    float sceneZ  = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float mine    = lineariseDepth(gl_FragCoord.z);
    float scene   = lineariseDepth(sceneZ);
    float visible = (sceneZ >= 1.0 || mine <= scene * 1.002 + 0.002) ? 1.0 : 0.0;
    outMask       = vec4(1.0, visible, 0.0, 1.0);
}
