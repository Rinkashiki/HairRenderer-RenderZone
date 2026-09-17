#ifndef SCENE_LOADER_H
#define SCENE_LOADER_H

#include <engine/core.h>
#include <engine/systems.h>

#include <functional>
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

// renderer.dof block, verbatim (see SCENE.md). Only meaningful when `set`.
struct DoFSettings {
    bool  set           = false;
    bool  enabled       = true;
    float focusDistance = 3.0f;
    float focusRange    = 0.5f;
    float nearBlurScale = 6.0f;
    float farBlurScale  = 6.0f;
    float maxCoC        = 16.0f;
};

struct LoadResult {
    Core::Scene*  scene           = nullptr;
    Core::Camera* camera          = nullptr;
    Core::Mesh*   primaryAnimated = nullptr; // first mesh that received an animation
    Vec4          clearColor      = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    std::vector<HairBindRequest> hairBindings; // meshes declaring bind_to

    // The scene's renderer hooks, recorded so a caller that loads on a worker
    // thread (HairViewer) can apply them on the main thread once the load has
    // joined — both touch pass state, so they must not run while the renderer
    // is being initialised or rendering. Also applied directly when a
    // `renderer` is passed to load_scene_json (SLViewer's synchronous path).
    std::string sssScatterLut; // absolute path, empty if not declared
    DoFSettings dof;
};

// Optional progress sink for load_scene_json. `fraction` is monotonic in [0,1];
// `stage` is a short human-readable line ("Decoding T-Alex-BaseColor"). Called
// from whatever thread runs the load, so the receiver must be thread-safe.
using ProgressFn = std::function<void(float fraction, const std::string& stage)>;

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
                        and renderer.dof are applied via the ForwardRenderer cast
                        (they are always recorded in LoadResult regardless).
    progress            Optional. Receives coarse progress (per mesh file, per
                        decoded GLB image) for a loading screen.

  Throws std::runtime_error on missing required fields or file-open failure.
  Tolerates unknown keys (logs a warning and ignores them).
*/
LoadResult load_scene_json(const std::string&     scenePath,
                           const std::string&     resourcesPath,
                           const std::string&     engineResourcesPath,
                           const std::string&     animationOverride = "",
                           Systems::BaseRenderer* renderer          = nullptr,
                           const ProgressFn&      progress          = nullptr);

} // namespace scene_loader

#endif
