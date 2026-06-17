#ifndef SCENE_LOADER_H
#define SCENE_LOADER_H

#include <engine/core.h>
#include <engine/systems.h>

#include <optional>
#include <string>
#include <vector>

USING_VULKAN_ENGINE_NAMESPACE

namespace scene_loader {

// A hair mesh that declared `bind_to` (and optionally `binding`). The scene
// loader only resolves the head pointer + sidecar path; the application builds
// the actual HairBinder (so hair_binding stays out of SLViewer's link).
struct HairBindRequest {
    Core::Mesh* hair        = nullptr;
    Core::Mesh* head        = nullptr;
    std::string bindingPath; // absolute sidecar path, empty if none declared
};

struct LoadResult {
    Core::Scene*  scene           = nullptr;
    Core::Camera* camera          = nullptr;
    Core::Mesh*   primaryAnimated = nullptr; // first mesh that received an animation
    Vec4          clearColor      = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    std::vector<HairBindRequest> hairBindings; // meshes declaring bind_to
};

// MSAA is baked into renderpasses/pipelines at create_passes(), so it has to be
// known BEFORE the renderer is constructed. This light-weight peek opens the
// scene JSON and reads only "renderer.msaa" — caller applies it to
// RendererSettings before instancing the renderer. Returns nullopt when the
// field is absent (caller should keep its default).
std::optional<MSAASamples> peek_msaa(const std::string& scenePath);

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
