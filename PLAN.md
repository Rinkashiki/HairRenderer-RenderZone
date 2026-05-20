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
