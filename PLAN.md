# Plan

Forward-looking work for this project. Completed features are tracked in git history; reference docs live in `CLAUDE.md`, `ANIMATION.md`, and `SCENE.md`.

---

## Open

### SLViewer — Windows clean-machine deploy validation

Local Windows smoke test now passes (see Done below). **Still not validated on a clean Windows host** (no Vulkan SDK, no VS), and there is a known blocker for that step:

- `CMakeLists.txt:247-264` only looks for `vulkan-1.dll` at `%VULKAN_SDK%\Bin\vulkan-1.dll`. The LunarG SDK 1.4.x **no longer ships a redistributable `vulkan-1.dll` in the SDK tree** — the installer puts the loader in `C:\Windows\System32\vulkan-1.dll` instead. Result: configure emits the "vulkan-1.dll not found" warning and `cmake --install` produces a folder **without** the loader. It runs fine locally (System32 loader is found) but will fail on a machine with no Vulkan runtime.
- Fix candidates (not yet applied — needs decision): extend the search to `%VULKAN_SDK%\runtime\x64`, then `%SystemRoot%\System32\vulkan-1.dll`; optionally escalate the WIN32 "not found" case to a `FATAL_ERROR` so a broken distributable can't be silently produced.

Remaining steps once the bundling is fixed:
1. Reconfigure/rebuild/install so `vulkan-1.dll` lands in `C:\SLViewer-windows\`.
2. Copy the folder to a clean Windows host (no Vulkan SDK, no VS, no IDE).
3. Run `SLViewer.exe test_anim.json` and confirm `test_anim.mp4` is produced, process exits 0, temp dir cleaned.

---

## Done

### Hair phantom shadows + rotation-dependent shadow shape (2026-05-26)

Hair self-shadowing produced shadows where no occluders existed, and the shadows changed size/position as the hair rotated with the head joint during animation. Isolated to the voxel cone-marching path (`computeHairShadowCone` in `hair_strand_epic.glsl`) — setting `material.advShadows = 0` made the artifact disappear.

**Root cause:** classic AABB-under-rotation bug. Two sites computed the hair's world-space AABB by transforming **only the two diagonal corners** of the local AABB:

```cpp
objectData.maxCoord = model * Vec4(boundingVolume->maxCoords, 1.0);
objectData.minCoord = model * Vec4(boundingVolume->minCoords, 1.0);
```

That's correct for translation + uniform scale but degenerate under rotation — the transformed `min`/`max` stop being the extrema of the rotated box. Worst around 45°, where the diagonal-corner approximation deviates most. With the hair parented to the head joint via `JointAttachment`, `get_model_matrix()` returns a rotating matrix every frame, so the "AABB" warped with rotation and the world↔voxel mapping shifted per frame. Writer (voxelization) and reader (cone-marching) used the same broken bounds, so density was written and read at warped texel locations — phantom shadows.

**Fixes shipped:**

- `ext/Vulkan-Engine/src/core/resource_manager.cpp::update_object_data` — transform all 8 corners of the local AABB by the model matrix, then componentwise min/max. Also fix `volumeCenter` to be in world space (the hair shader's `fakeNormal = normalize(g_modelPos - volumeCenter)` expects world coords; it was getting local).
- `ext/Vulkan-Engine/src/core/passes/hair_voxelization_pass.cpp` — same 8-corner fix for the skull-occluder voxelization step, so writer and reader stay consistent.

**Verified:** user confirmed phantom shadows and rotation-induced shadow drift gone after the fix.

---

### Hair pink/magenta flash on certain camera angles (2026-05-26)

Hair would briefly flash a magenta-and-white pattern at certain camera angles (HairViewer only — SLViewer was clean). Bisected with progressively-narrowing shader overrides: a bright-red override at the *end* of `hair_strand_epic.glsl::main()` was apparently being ignored, but the same override at the *top* with an early `return` worked. That ruled out everything in the post-process chain and isolated the bug to the hair fragment shader itself — `color` was being poisoned with NaN/Inf *before* the override line.

**Root cause:** `computeAmbient()` (in `hair_strand_epic.glsl`) declared `vec3 ambient;` and only assigned it in the `else` branch. The `if (scene.useIBL)` branch computed `rotatedNormal` but never set `ambient`. GLSL doesn't zero-initialize locals, so the function returned whatever happened to be in the register — often NaN. Then `color += ambient` poisoned the output. Nadia's scene has `use_ibl: true`, so the bad path ran every frame; whether the register held a NaN at that moment depended on what the BSDF math had just done, which is why it looked angle-correlated.

**Fixes shipped (all in `ext/Vulkan-Engine/resources/shaders/`):**

- `forward/hair_strand_epic.glsl::computeAmbient` — initialize `ambient` to the simple ambient term in both branches; left a TODO in the IBL branch for the actual cubemap sampling that was meant to live there.
- `forward/hair_strand_epic.glsl::main` — defensive guard at the end of main: `if (any(isnan|isinf(color))) color = vec3(0.0); color = clamp(color, 0, 64);` Kept (per user) as belt-and-braces against future degeneracies; cost is one isnan + one clamp per fragment.
- `forward/hair_strand_epic.glsl::buildBasis` — `cross(T, V)` is zero when the strand tangent is parallel to the view direction (a real on-screen case when a strand points at the camera); `normalize(zero)` was NaN. Fall back to a stable arbitrary perpendicular axis. Affects the glints path via `computeMicroTangent`.
- `scripts/BRDFs/epic_hair_BSDF.glsl::evalEpicHairBSDF / evalHairBSDF` — clamp `cosThetaD = max(cosThetaD, 1e-3)`. This is `cos(0.5 * |asin(sinThetaV) - asin(sinThetaL)|)`, which goes to 0 when the strand aligns with V on one side and with L on the other. Multiple downstream terms divide by `cosThetaD` or use it as a `pow` exponent.
- `scripts/BRDFs/epic_hair_BSDF.glsl::computeDualScatteringTerms` — clamp `af.r + af.g + af.b` and `ab.r + ab.g + ab.b` denominators to `1e-5`, and clamp the `sigma_b` denominator. The hair scatter LUT can return zero at corners, which used to produce NaN through `af_weights`/`ab_weights`/`sigma_b`.
- `scripts/BRDFs/epic_hair_BSDF.glsl::evalKajiyaKayDiffuseAttenuation` — projection `V - N * dot(V, N)` is zero when V is parallel to N (the strand tangent); same fallback-to-arbitrary-perpendicular fix as `buildBasis`.

**Also in this initiative (still kept even though it turned out not to be the pink-flash cause):**

- `ext/Vulkan-Engine/src/core/resource_manager.cpp::update_object_data` — the entire ObjectUniforms/MaterialUniforms/VBO upload block was gated on `inFrustum`. Now mirrors the BLAS rule: a still-active, ray-hittable mesh always gets its uniforms refreshed regardless of frustum visibility. Eliminates a real class of staleness bug (a hair mesh briefly leaves frustum during fast orbit → its uniform slot keeps last frame's data) even if it wasn't what caused this specific symptom.

**Verified:** User confirmed pink/black flashes gone after the `computeAmbient` fix. Earlier BSDF clamps were verified by progressive testing — each kept after confirming pink persisted but black flashes appeared (the safety net catching NaN that the clamps hadn't yet addressed), proving each NaN source was real even if not the dominant one.

**Lighting issue (too dark + wrong specular)** — the same BSDF degeneracy fixes plausibly resolve this; user to verify next session, see the Open entry above.

---

### Scene definition → JSON (2026-05-20)

Replaced the duplicated hardcoded `setup()` bodies in `src/application.cpp` (HairViewer) and `src/slviewer/application_sl.cpp` (SLViewer) — previously a `#ifdef USE_GLB_MODELS / LOAD_ALEX|JAVI|MARIA|NADIA / USE_NEURAL_MODELS` ladder — with a JSON scene format loaded at runtime. Both viewers ship `resources/scenes/default.json` (current Alex configuration); SLViewer adds `--scene <path>`. The old C++ paths remain compiled behind `#define USE_HARDCODED_SCENE` as a fallback.

**Shipped:**
- `src/scene_loader.{h,cpp}` — application-layer parser (linked by both targets). Broad schema covering all material classes (`pbr`/`haircard`/`hairepic`/`hair`/`hairdisney`/`unlit`), `point`/`directional` lights, mesh types `glb`/`obj`/`ply`/`hair`/`neural_hair`, nested `children`, skybox, ambient, fog, SSS LUT. Tolerates unknown keys (warn), hard-fails on missing required ones.
- `resources/scenes/default.json` (Alex) plus `javi.json`, `maria.json`, `nadia.json`, `neural_tono.json`, `bust_strands.json` — exercise the broad schema across all variants previously gated by `#ifdef`s.
- SLViewer CLI: `SLViewer <animation.json> [--scene S.json] [other flags]`. Animation arg always overrides the first mesh's `animation` field (or the first skinned mesh if none has one); warning if neither exists.
- `SCENE.md` documents the full schema; `CLAUDE.md` updated.
- CMake: `scene_loader.cpp` + `hair_loader.cpp` explicitly added to SLViewer source list; `resources/scenes` directory installed.

**Issues found + decisions:**
- *Animation in two places confused the override semantics.* Resolved by making the per-mesh `animation` field **optional** (option b). SLViewer-targeted scenes can omit it; HairViewer scenes (and `default.json`) keep it because HairViewer has no animation CLI flag.
- *GLB-embedded textures need a sentinel.* Materials reference GLB-embedded textures via `"$GLB[N]"` (resolves to the Nth element of the GLB's `outTextures` vector). Plain strings are paths relative to the resources root.
- *Children support added late.* Neural avatars parent hair to head — required nesting. `build_mesh` now recurses through `children`, transforms inherit through the engine's existing `Object3D` hierarchy.
- *Frame budget for SLViewer.* When the animation comes from the CLI override (not the scene), the loader doesn't surface duration/fps to SLViewer. Resolved by re-reading the animation JSON header (a few hundred bytes) in `SLApplication::setup()`.

**Verified:** Debug `HairViewer --frames 10 --log-level warn` produced an empty `debug_trace.log` (clean). Release `SLViewer test_anim.json` renders 120 frames and encodes a valid MP4. `--scene javi.json|maria.json|nadia.json|neural_tono.json|bust_strands.json` all load and render without errors.

---

### SLViewer — Windows ffmpeg export fix + local smoke test (2026-05-19)

`SLViewer.exe` rendered all frames but failed at the encode step with *"El nombre de archivo... no son correctos"* (Windows ERROR_INVALID_NAME) and `ffmpeg exited with code 1`.

**Issues found:**
1. `resource_paths.cpp` `get_ffmpeg_path()` looked for a bundled `ffmpeg` with no extension; the actual file is `ffmpeg.exe`, so `std::filesystem::exists()` failed and it fell back to bare `"ffmpeg"` (not on PATH).
2. `video_encoder.cpp` built a Windows command starting with a quote and containing several quoted tokens. `std::system()` runs `cmd.exe /c <str>`; cmd strips the outermost quote pair and corrupted the executable path.

**Solutions implemented:**
1. `get_ffmpeg_path()` now picks `ffmpeg.exe` on `_WIN32` (and falls back to that name for PATH lookup).
2. `VideoEncoder::encode()` wraps the entire Windows command in one extra pair of double quotes so the inner quoting survives `cmd /c`.

**Verified:** Release rebuild + `cmake --install` → `C:\SLViewer-windows`; `SLViewer.exe test_anim.json --output test_anim.mp4` produced a 232 KiB / 120-frame MP4, ffmpeg exit 0, frames sourced from `%TEMP%\slviewer_<pid>\`.

---

## Upcoming

_Add new initiatives below as they come up._
