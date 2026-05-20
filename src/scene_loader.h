#ifndef SCENE_LOADER_H
#define SCENE_LOADER_H

#include <engine/core.h>
#include <engine/systems.h>

#include <string>
#include <vector>

USING_VULKAN_ENGINE_NAMESPACE

namespace scene_loader {

struct LoadResult {
    Core::Scene*  scene           = nullptr;
    Core::Camera* camera          = nullptr;
    Core::Mesh*   primaryAnimated = nullptr; // first mesh that received an animation
    Vec4          clearColor      = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
};

/*
  Load a scene from JSON. See SCENE.md for the schema.

    scenePath           Absolute path to the scene JSON.
    resourcesPath       Trailing-slash root for resolving relative mesh/texture paths.
    engineResourcesPath Trailing-slash root for engine built-ins (sphere.obj, etc.).
    animationOverride   If non-empty, replaces the animation on the first mesh that
                        declares one. If no mesh declares an animation, the first
                        mesh with skinning data receives it.
    renderer            Optional. When non-null, the scene's renderer.sss_scatter_lut
                        is applied via the ForwardRenderer cast.

  Throws std::runtime_error on missing required fields or file-open failure.
  Tolerates unknown keys (logs a warning and ignores them).
*/
LoadResult load_scene_json(const std::string&     scenePath,
                           const std::string&     resourcesPath,
                           const std::string&     engineResourcesPath,
                           const std::string&     animationOverride = "",
                           Systems::BaseRenderer* renderer          = nullptr);

} // namespace scene_loader

#endif
