#shader vertex
#version 460
// Selection outline mask — triangle meshes. See OutlineMaskPass.
#include camera.glsl
#include object.glsl

layout(location = 0) in vec3 pos;

void main() {
    gl_Position = camera.viewProj * object.model * vec4(pos, 1.0);
}

#shader fragment
#version 460
#include camera.glsl

// Forward pass LinearDepth (resolved): gl_FragCoord.z of the visible surface,
// cleared to 1.0 where nothing was drawn.
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(location = 0) out vec4 outMask;

void main() {
    float sceneZ = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    // Compare in linear depth with a small relative tolerance: the object drew
    // itself into sceneDepth at (almost) the same z, and MSAA-resolved edge texels
    // are averages that only ever push the scene *farther*.
    float mine    = lineariseDepth(gl_FragCoord.z);
    float scene   = lineariseDepth(sceneZ);
    float visible = (sceneZ >= 1.0 || mine <= scene * 1.002 + 0.002) ? 1.0 : 0.0;
    outMask       = vec4(1.0, visible, 0.0, 1.0);
}
