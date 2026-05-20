# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Configure (from repo root)
mkdir build && cd build
cmake ..

# Build (from build/)
cmake --build .

# With Ninja (recommended)
cmake -G Ninja .. && ninja
```

Prerequisites: Vulkan SDK 1.3.* (with VMA and Shaderc), CMake, Git LFS (resources stored via LFS). C++17.

## Architecture Overview

This is a **Vulkan strand-based hair renderer** built on top of a custom Vulkan engine (`ext/Vulkan-Engine/`). The application (`src/`) links against the engine as a library.

### Two-Layer Structure

- **Application layer** (`src/`): `HairViewer` (in `application.h/cpp`) orchestrates the lifecycle — scene setup, camera, lights, materials, neural hair loading. GUI lives in `gui.h/cpp`. Hair-specific asset loading in `hair_loader.h/cpp`.
- **Engine layer** (`ext/Vulkan-Engine/`): Contains the renderer, render passes, materials, RHI (Graphics/), resource management, and shader system. Headers in `include/engine/`, implementations in `src/`.

### Renderer — Forward Pipeline

The renderer is **forward** (not deferred). The `ForwardRenderer` (`systems/renderers/forward.h/cpp`) defines this pass sequence:

| Index | Pass | Type | Purpose |
|-------|------|------|---------|
| 0 | `SHADOW_PASS` | Variance shadow map | Per-light shadow maps |
| 1 | `HAIR_SCATTER_PASS` | Compute | Hair scattering LUT |
| 2 | `HAIR_VOXELIZATION_PASS` | Compute | Hair volume density |
| 3 | `FORWARD_PASS` | Rasterization (MSAA+MRT) | Main scene rendering |
| 4 | `SSAO_PASS` | Post-process | Ambient occlusion + thickness |
| 5 | `SSS_PASS` | Post-process | Subsurface scattering |
| 6 | `BLOOM_PASS` | Post-process | Physically-based bloom (Jimenez 2014) |
| 7 | `TONEMAPPIN_PASS` | Post-process | HDR tonemapping |
| 8 | `FXAA_PASS` | Post-process | Optional software AA |

Hair rendering uses the forward path because hair fibers benefit from hardware MSAA — TAA is insufficient for fine strands.

### Adding a New Post-Process Pass

1. **Create the pass class**: Inherit from `PostProcessPass` (or `BasePass` for full control). See `postprocess_pass.h/cpp` for the simplest template — it takes a shader path, binds one input image, and draws a fullscreen quad (`m_vignette`).

2. **Write the shader**: Place in `ext/Vulkan-Engine/resources/shaders/`. Use the unified file format with `#shader fragment` directives. Shaders compile on the fly (no offline compilation needed).

3. **Register in `ForwardRenderer::create_passes()`** (`ext/Vulkan-Engine/src/systems/renderers/forward.cpp`):
   - Add an enum value to `RendererPasses`
   - Resize `m_passes` to accommodate the new slot
   - Instantiate the pass (see tonemapping/FXAA as examples)
   - Set the dependency table via `set_image_dependace_table()` — this links output images from previous passes as inputs

4. **Dependency table format**: `{iVec2(SOURCE_PASS_INDEX, FRAMEBUFFER_INDEX), {ATTACHMENT_IDS...}}` — this tells the engine which framebuffer attachments from a source pass to feed into the new pass via `link_previous_images()`.

5. **Default pass**: The last pass in the chain that renders to the swapchain must have `isDefault = true`. If inserting a pass before the current final pass, update which pass is the default.

### Render Pass Lifecycle (IBasePass)

Each pass implements four virtual setup methods called during `setup()`:
- `setup_attachments()` — declare render targets and subpass dependencies
- `setup_uniforms()` — create descriptor pool, layouts, and allocate descriptor sets
- `setup_shader_passes()` — build graphics/compute pipelines, link shaders
- `render()` — record command buffer for this pass

Resources flow between passes through the dependency table + `link_previous_images()`. There is **no render graph** — passes are manually sequenced.

### Shader System

- **No reflection** — descriptor layouts must be manually defined in C++ pass code
- **Unified files** — vertex + fragment in one `.glsl` file, separated by `#shader fragment`
- **Simple includes** — `#include utils.glsl` (non-recursive, declared in entry-point shader)
- **Include scripts** — reusable modules in `resources/shaders/scripts/` (BRDFs, lighting, camera, etc.)
- **On-the-fly compilation** via Shaderc — no offline `.spv` generation needed

### Key Engine Concepts

- **Resource Manager** (`core/resource_manager.h/cpp`): CPU-to-GPU data upload. Holds shared resources like `VIGNETTE` (fullscreen quad mesh used by all post-process passes).
- **Materials**: `HairEpicMaterial` for hair, `PhysicallyBasedMaterial` for head/eyes. Material classes define their own descriptor layouts and uniform buffers.
- **RHI** (`Graphics/` folder): Low-level Vulkan abstraction (device, swapchain, command buffers, images, descriptors). Should generally remain untouched.
- Uses classic `VkRenderPass` objects, **not** `VK_KHR_dynamic_rendering`.

### Asset Loading

All loading goes through `Tools::Loaders::load_3D_file()` (`ext/Vulkan-Engine/src/tools/loaders.cpp`), which dispatches by file extension:

| Format | Extension | Library | Use |
|--------|-----------|---------|-----|
| OBJ | `.obj` | tinyobj_loader | Generic meshes (engine built-ins, lights) |
| PLY | `.ply` | tinyply | Head, eye, and neural-hair meshes |
| HAIR | `.hair` | Custom binary | Strand hair geometry |

`FBX` is defined in `common.h` but **not implemented**.

**Hair loading** has two paths:
- **Standard strands** (`.hair`): `load_hair()` reads the custom binary format (4-byte `"HAIR"` signature, header, packed point/segment arrays). Converts strand segments to triangulated geometry with tangents.
- **Neural hair** (`.ply`): `hair_loaders::load_neural_hair()` (`src/hair_loader.cpp`) parses PLY with tinyply, then applies strand-aware processing (100 verts/strand, tangent = direction to next vertex, random per-strand UV/color).

`load_3D_file()` accepts an `asynCall` flag (default `true`). Most loads in `application.cpp` pass `false` (synchronous). Neural hair is detached onto its own `std::thread`.

Loaded data flows: `Vertex[]` + `uint32_t[]` → `Core::Geometry::fill()` → `Core::Mesh::push_geometry()` → scene.

### Animation format

The animations loaded are in json format. The specifics of this format and its structure are defined in detail in @ANIMATION.md

### Scene format

Scenes are defined in JSON and loaded at runtime by `src/scene_loader.{h,cpp}`. Schema, material types, light types, animation binding rules, and worked examples live in @SCENE.md. The bundled default is `resources/scenes/default.json`; alternative scenes (`javi`, `maria`, `nadia`, `neural_tono`, `bust_strands`) exercise the broader schema. Both viewers retain their hardcoded `setup()` behind `#define USE_HARDCODED_SCENE` as a fallback.

### Scene Selection

Scenes are JSON-driven (`resources/scenes/*.json` — see @SCENE.md).

- **HairViewer** loads `resources/scenes/default.json` unconditionally. To use a different scene, either edit that file or change the path in `src/application.cpp::HairViewer::setup()`.
- **SLViewer** accepts an optional `--scene <path>` flag; default falls back to `resources/scenes/default.json`.

The previous C++ `#ifdef USE_GLB_MODELS / LOAD_ALEX|JAVI|MARIA|NADIA / USE_NEURAL_MODELS` ladder is retained behind `#define USE_HARDCODED_SCENE` as a fallback (see top of `src/application.cpp` and `src/slviewer/application_sl.cpp`). Default builds use the JSON path.

#### Engine example applications

The engine ships four standalone demos under `ext/Vulkan-Engine/examples/`. They are **disabled by default** in the main build (`set(BUILD_EXAMPLES OFF ...)` in root `CMakeLists.txt`). To build them:

```bash
cmake -DBUILD_EXAMPLES=ON ..
cmake --build .
```

This produces four separate executables alongside `HairViewer`:

| Executable | Scene | Renderer |
|------------|-------|----------|
| `RendererApp` | General textured PBR scene, raytracing enabled | ForwardRenderer |
| `LightingTest` | 10×10 grid of 100 point lights, stress test | DeferredRenderer |
| `RaytracingApp` | Environment with torii/tower/droid, raytraced shadows | DeferredRenderer |
| `RotatingKabuto` | Single Kabuto mesh rotating in place | ForwardRenderer |

Each example has its own `main.cpp` + `application.cpp/h` under `ext/Vulkan-Engine/examples/<name>/`. They share the same `-aa`, `-gui` CLI flags as HairViewer but use `EXAMPLES_RESOURCES_PATH` for assets (separate resource directory under `ext/Vulkan-Engine/examples/resources/`).

**No base class is shared** — `HairViewer` and the `Application` classes in the examples are independent parallel implementations of the same lifecycle pattern (`init → setup → tick loop → shutdown`).

## Workflow

The workflow for this project will be mainly driven by the user, and Claude will execute the proposed tasks. But Claude will always double-check the orders and ask any inquiries to the user before performing a task. After finishing a task, Claude will summarize the results and ask the user for validation, and automatically update the PLAN.md to mark the task as done, and summarize the issues found and solutions implemented. It should also update CLAUDE.md if necessary.

**Before writing significant code**, Claude should ask the user questions to validate the plan. Don't assume — ask. **Plans should be fool-proof**, so Claude should make the necessary amount of questions to the user before committing to the plan.

## Debug Test Harness

The project includes a debug test mode for validating the Vulkan pipeline without visual inspection. Claude should use this to verify changes that touch render passes, descriptors, or synchronization.

### Building for test

```bash
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
```

A Debug build enables Vulkan validation layers automatically (`NDEBUG` not defined).

### Running a validation test

```bash
# From build/
./HairViewer --frames 10 --log-level warn
```

This renders 10 frames, captures validation layer messages to `build/debug_trace.log`, then exits cleanly.

### CLI flags

| Flag | Values | Default | Description |
|------|--------|---------|-------------|
| `--frames N` | Any positive integer | No limit (interactive) | Auto-exit after N frames |
| `--log-level` | `error`, `warn`, `verbose` | `warn` | Vulkan validation message severity filter |
| `--log-file` | File path | `build/debug_trace.log` | Where to write the trace (auto-set when `--frames` is used) |

### Verbosity levels

| Level | Vulkan severities captured |
|-------|---------------------------|
| `error` | `ERROR` only |
| `warn` | `WARNING` + `ERROR` |
| `verbose` | `VERBOSE` + `INFO` + `WARNING` + `ERROR` |

### Interpreting results

- **Empty or minimal trace** = clean run, no issues.
- **Errors** = must fix. Typical causes: descriptor mismatches, missing image layout transitions, synchronization issues.
- **Warnings** = fix if from new code, document if pre-existing.
- After fixing, re-run and verify the trace is clean.

### Key files

- Debug callback: `ext/Vulkan-Engine/include/engine/graphics/utilities/utils.h` (`debugCallback`)
- Severity filter wiring: `ext/Vulkan-Engine/src/graphics/utilities/utils.cpp`
- Logger: `ext/Vulkan-Engine/thirdparty/logger/include/logger.h`
- CLI parsing + frame limit: `src/main.cpp`, `src/application.h/cpp`

## Building the SLViewer Distributable

The distributable is produced by building in Release mode and running `cmake --install`. The result is a self-contained folder that can be copied to any machine without a Vulkan SDK, IDE, or source tree.

### Linux

```bash
# 1. Configure and build (Release defines NDEBUG — disables validation layers)
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target SLViewer -j$(nproc)

# 2. Install into a distribution folder
cmake --install . --prefix ~/SLViewer-linux
```

The installed layout:

```
SLViewer-linux/
├── SLViewer              # ELF binary
├── ffmpeg                # bundled static ffmpeg GPL build (BtbN linux64)
├── THIRD_PARTY_NOTICES.txt
└── resources/
    ├── shaders/          # full engine shader tree (compiled on-the-fly)
    ├── meshes/           # engine built-in meshes (sphere, cube)
    ├── textures/         # engine LUTs + IBL maps
    ├── models/alex/      # alex.glb, hair_fauxmohawk.obj
    ├── textures/alex/    # hair data + tangent textures
    └── animations/       # test_anim.json, test_morph.json
```

`libvulkan.so.1` is **not bundled**. On Linux the Vulkan loader ships with GPU drivers (`mesa-vulkan-drivers`, `nvidia-driver`, etc.) and is always present on any machine capable of running Vulkan. Bundling an SDK copy of the loader breaks on other machines because the SDK loader looks for ICDs in SDK-specific paths that don't exist there.

**Important**: use `CMAKE_BUILD_TYPE=Release` for distribution. A Debug build requests Vulkan validation layers, which are SDK-only and unavailable on target machines.

### Windows

```bat
rem 1. Configure and build (MSVC; /MT static runtime is set automatically)
cd build
cmake -G "Visual Studio 17 2022" ..
cmake --build . --config Release --target SLViewer

rem 2. Install into a distribution folder
cmake --install . --config Release --prefix C:\SLViewer-windows
```

The installed layout:

```
SLViewer-windows\
├── SLViewer.exe          # statically linked MSVC runtime (/MT — no VC++ redist needed)
├── ffmpeg.exe            # bundled static ffmpeg GPL build (BtbN win64)
├── vulkan-1.dll          # Vulkan loader from %VULKAN_SDK%\Bin\
├── THIRD_PARTY_NOTICES.txt
└── resources\            # same tree as Linux
```

`vulkan-1.dll` is located automatically from `%VULKAN_SDK%\Bin\` at configure time. If the env var is not set, CMake emits a warning and the DLL must be copied manually. The MSVC runtime is compiled in statically (`/MT`), so no VC++ Redistributable is required on the target machine.

**Note**: The ffmpeg `.zip` for Windows is downloaded at configure time (same as Linux). If the download fails, system `ffmpeg` on `PATH` is used as a fallback at runtime.

## SLViewer — Headless Video Export

`SLViewer` renders the Alex scene from a JSON animation file and encodes the result as an MP4 using system ffmpeg.

### Prerequisites

- `ffmpeg` must be on `PATH` (`sudo apt install ffmpeg` on Ubuntu).
- A Vulkan-capable GPU (no display required — window is created hidden).

### Usage

```bash
# From build/
./SLViewer <animation.json> [options]
```

| Argument | Default | Description |
|----------|---------|-------------|
| `<animation.json>` | (required) | Path to the timeline JSON. Overrides the scene's `animation` field on the first mesh that declares one (or on the first skinned mesh). |
| `--scene <file.json>` | `resources/scenes/default.json` | Scene JSON. Schema in @SCENE.md. |
| `--output <file.mp4>` | `output.mp4` | Output video path |
| `--width N` | 1920 | Render width in pixels |
| `--height N` | 1080 | Render height in pixels |
| `--keep-frames` | off | Retain the per-frame PNG dump in the temp dir after encoding |
| `--log-level error\|warn\|verbose` | `warn` | Vulkan validation message severity filter |

### Example

```bash
./SLViewer ../resources/animations/test_anim.json \
           --output test_output.mp4 \
           --width 1280 --height 720 \
           --log-level warn
```

This renders 120 frames (4 s × 30 fps) headlessly, writes PNGs to a temp dir (`std::filesystem::temp_directory_path()/slviewer_<pid>/` — e.g. `/tmp/slviewer_<pid>/` on Linux, `%TEMP%\slviewer_<pid>\` on Windows), encodes them with ffmpeg (`libx264`, `yuv420p`, `crf 18`), and deletes the temp dir on success.

### Notes

- If ffmpeg fails, the temp PNG dump is retained automatically for diagnosis.
- The resources path (models, textures, HDR) is auto-discovered relative to the executable via `discover_resources_path()` — no extra flags needed.

## AMASS → Animation JSON Converter

`tools/amass_to_json.py` converts AMASS / SMPL-X motion-capture `.npz` files
into the engine's animation JSON (see `ANIMATION.md`).

The Alex GLB rig is a verbatim SMPL-X 55-joint skeleton (identical joint names
and ordering), so conversion is a **direct skeletal retarget** — no joint
remapping. The script is **numpy-only**: it does not need the SMPL-X model,
identity betas, `torch`, or the `smplx` package (those only generate mesh
vertices; the engine does the skinning from the emitted joint tracks). AMASS
body mocap carries no facial expression animation, so no `morph:` tracks are
produced.

### Prerequisites

- Python 3 with `numpy`. No other dependencies.

### Usage

```bash
# Single file (writes Stefanos_....json next to the .npz)
python3 tools/amass_to_json.py path/to/clip_stageii.npz

# Explicit output path, into the engine's animations dir
python3 tools/amass_to_json.py clip_stageii.npz resources/animations/dance.json

# Batch a directory tree (recursive); mirrors structure into the out dir
python3 tools/amass_to_json.py amass_DanceDB/ resources/animations/danceDB/
```

| Argument / flag | Default | Description |
|-----------------|---------|-------------|
| `INPUT` | (required) | An AMASS `.npz`, or a directory (batch all `*.npz`, recursive). |
| `OUTPUT` | next to input | `.json` path, or a directory to write into. |
| `--channels body,hands,face` | all three | Comma list of joint groups to emit. `body` also covers the pelvis. |
| `--fps N` | `30` | Output sample rate. Source (usually 120 fps) is decimated; real wall-clock timing is preserved. |
| `--no-root-motion` | off (root motion **on**) | Drop the `joint:pelvis/translation` track (animate in place). |
| `--no-up-convert` | off (convert **on**) | Skip the Z-up→Y-up root rotation. AMASS is Z-up; the rig is Y-up — leave conversion on unless the character appears lying down. |
| `--loop` | off (`"loop": false`) | AMASS clips are one-shot; set this to loop playback. |
| `--keep-static` | off | Keep rotation tracks that never leave the rest pose (default drops them to shrink files). |
| `--name NAME` | input file stem | Override the animation `name` field. |

### Notes

- DanceDB folders contain a `*_stagei.npz` **shape-only** identity file (no
  motion) alongside the `*_stageii.npz` motion clips. The shape files have no
  `trans`/pose arrays; batch mode **skips them with a warning** — this is
  expected, not an error.
- Both AMASS layouts are handled: split arrays (`root_orient`, `pose_body`,
  `pose_hand`, `pose_jaw`, `pose_eye`) when present, else the SMPL-X
  `poses` (165) block is sliced.
- Files are dense per-frame mocap: an 80 s clip at 30 fps with hands ≈ several
  MB. Shrink with `--channels body`, a lower `--fps`, or `--no-root-motion`.
- Output is sanity-checkable: every rotation key is `[t, [x,y,z,w]]` with a
  unit-norm quaternion; load any result with `SLViewer` or the interactive
  `HairViewer` to verify visually.