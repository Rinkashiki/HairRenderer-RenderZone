#pragma once
// CPU viewport picking: casts the ray under the mouse against every active mesh
// in the scene and returns the nearest hit.
//
// - Triangle meshes are tested against their current CPU vertices (deformed
//   ones when the mesh is morph/skin animated), so animated characters pick
//   where they are drawn, not at their rest pose.
// - Strand hair (line geometry) is tested per segment with a screen-space
//   tolerance, since a fiber is far thinner than a pixel. Surface-bound hair
//   uses the binder's reconstructed strands.
// - A light's dummy marker mesh resolves to the light itself.

#include <vector>

#include <engine/core.h>

#include "hair_binding.h"

namespace picking {

struct Ray {
    Vec3 origin{0.0f};
    Vec3 dir{0.0f, 0.0f, -1.0f}; // normalized
};

struct Hit {
    Core::Object3D* object{nullptr}; // what should be selected (mesh, or its light)
    Core::Mesh*     mesh{nullptr};   // the mesh that was actually hit
    float           distance{0.0f};  // along the ray, world units
};

// World-space ray through a pixel. `mousePx` / `viewportPx` in the same units
// (ImGui display coordinates).
Ray screen_ray(Core::Camera* camera, Vec2 mousePx, Vec2 viewportPx);

// Nearest object under the ray; Hit::object == nullptr when nothing was hit.
Hit pick(Core::Scene*                                  scene,
         Core::Camera*                                 camera,
         Vec2                                          mousePx,
         Vec2                                          viewportPx,
         const std::vector<hair_binding::HairBinder*>& binders,
         float                                         strandTolerancePx = 4.0f);

} // namespace picking
