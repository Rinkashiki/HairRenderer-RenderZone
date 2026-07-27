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

**Hair shadowing (deliberate setup, see `hair_strand_epic.glsl` + `variance_shadow_pass.cpp`):** the hair is **not** rendered into the variance shadow map, and its **voxel cone-trace self-shadow is disabled**. Both produced dark, light-dependent streaks/wedges on the hair (hair-as-lines self-shadow acne in the VSM; coarse-256³ path-integral wedges in the voxel cone trace). The hair now takes occlusion from the smooth VSM `solidOcclusion` **only** — so the head/body still shadow the hair, but hair-on-hair self-shadow is off. Trade-offs / how to restore: hair no longer casts a shadow-map shadow onto the face/body (re-add via VSM receiver-bias tuning), and hair-on-hair self-shadow needs a higher-resolution hair voxel volume (e.g. 512³) before the voxel path can be re-enabled cleanly. `computeHairShadow`/`hairShadow`/`computeHairShadowCone` remain defined but uncalled.

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
  - **`EyelashMaterial`** (subclass of `HairEpicMaterial`, type `HAIR_STR_EYELASH_TYPE`, scene JSON type `"eyelash"`): same params/uniforms/geometry as epic hair but routed to `shaders/forward/eyelash_strand.glsl`, which calls `evalEyelashBSDF` (in `epic_hair_BSDF.glsl`) instead of `evalEpicHairBSDF`. That variant wires up `R_power`/`TT_power`/`TRT_power`/`use_backlit` — which the epic path ignores — so eyelashes can be made less reflective (low `specular`/`R_power`) and more transmissive (higher `TT_power` + `use_backlit`) without affecting scalp hair. Any subsystem that special-cases epic hair (forward draw loop, VSM skip, hair voxelization, resource-manager voxel union, GUI widget) must test `IMaterial::is_epic_hair_family()`, not the exact type, or eyelashes drop out of it. It also carries a **lighting-model selector** — see "Eyelash Shading Models" below.
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

### Self-Contained Character GLBs (baked materials)

Character `.glb` files can carry their **own materials and textures** so the scene JSON only needs overrides (or nothing). The character GLBs (`nadia`, `alex`, `maria`, `javi`) are **baked** this way; their scene JSONs no longer declare `material` / `extra_materials` / `primitive_materials` for the character mesh.

**How a material is stored in the GLB (hybrid):**
- **Standard glTF slots** — baseColor, metallic-roughness, occlusion, normal, and factors — are written so the GLB opens sensibly in any glTF viewer. Roughness + Metallic + AO live in a single **ORM** image (R=occlusion, G=roughness, B=metallic), as glTF requires.
- **The full engine material block** is written verbatim into each glTF material's `extras.vkfw_material` (a JSON string — the same schema `build_pbr` consumes). Texture references are `$GLB[<image name>]`, or `$GLB[<image>:<r|g|b|a>]` for one channel of a packed atlas. **The engine reads this block**; the standard slots are decoration for external tools.

**Packed atlases (ORM / CS) cost one texture, not one per channel.** The characters ship two packed maps: **ORM** (R=AO, G=roughness, B=metallic) and **CS** (R=curvature, G=scattering). A `$GLB[<image>:<channel>]` reference in a slot the shader can swizzle — roughness, metallic, occlusion, curvature, scattering — resolves to the **whole** decoded image, shared by every slot that names it; the channel index travels to the GPU instead. `PhysicallyBasedMaterial` packs the five 2-bit indices into `dataSlot10.y` (`packedChannels` in `physically_based.glsl` — the old unused `_slot10_pad`, so the UBO layout is unchanged), and each sample site reads `texture(tex, uv)[channel]` rather than `.r`. Result: ORM + CS = 2 decoded 8K textures instead of 5 (~537 MB vs ~1.34 GB of VRAM per character). Slots that *can't* swizzle fall back to the old behaviour — unpack the channel into its own replicated grayscale texture. Un-suffixed references and loose single-channel maps are unaffected (channel defaults to R).

Source paths accept the same suffix, so a non-baked scene JSON can point straight at a packed map: `"roughness_texture": "textures/alex/T-Alex-ORM.png:g"`. `resolve_texture` caches loose paths per mesh, so the atlas is read once there too.

**Loader layering (per material slot, low→high priority)** — see `scene_loader.cpp::assemble_baked_materials`:
1. glTF standard fields
2. `extras.vkfw_material` (the baked base)
3. scene JSON `material` / `extra_materials[i]` — merged over the base via `merge_patch` (**JSON wins per key**)

So the final material equals the JSON wherever the JSON sets a field, and the GLB fills the rest. This makes it **fully backward-compatible**: an unbaked GLB (only baseColor, no `extras`) + a JSON that defines the whole material renders exactly as before (the loader takes the unchanged non-baked path when no geometry carries a baked block). A baked GLB + a slimmed JSON renders identically because the baked block holds the same params and the same (verbatim) texture bytes.

**Geometry→slot mapping.** With `primitive_materials` present the loader uses it (legacy behaviour preserved); otherwise each geometry uses **its own glTF material index** as the slot, so a fully-slimmed scene needs no slot mapping. The baker bakes each glTF material with the block of whatever JSON slot it resolved to, so multi-material characters (e.g. Javi's hearing-aid parts) reconstruct correctly without `primitive_materials`.

**Bake recipes — `tools/bake_recipes/<character>.json`.** Once a scene is slimmed its materials live only in the GLB, so there is nothing left for the baker to read. The recipes are the re-bakeable **source of truth** for each character's material: a small JSON holding just `material` / `extra_materials` / `primitive_materials`, with texture paths pointing at the loose maps under `resources/textures/<char>/`. Edit a recipe, re-run the baker, and the GLB is regenerated — this is how you iterate on a character's materials now.

**Baker — `tools/bake_glb_material.py`** (needs `pygltflib` + `Pillow`):
```bash
# Re-bake a character from its recipe (the normal path today).
# --in-glb must be the UNBAKED original: baking appends images, so baking on top of a
# baked GLB would strand the old ones as orphaned buffer data (the baker refuses unless --force).
git cat-file blob 286078a~1:resources/models/nadia/nadia.glb > /tmp/nadia.orig.glb
python tools/bake_glb_material.py resources/scenes/nadia.json \
    --materials tools/bake_recipes/nadia.json --in-glb /tmp/nadia.orig.glb --inplace

# First-time bake of a scene that still declares its materials (also slims the scene JSON):
python tools/bake_glb_material.py resources/scenes/nadia.json --inplace
# Safe dev output (writes <glb>.baked.glb + <scene>.baked.json, originals untouched):
python tools/bake_glb_material.py resources/scenes/nadia.json
```
It reads the material blocks (recipe if `--materials`, else the scene's `material` / `extra_materials` / `primitive_materials`), embeds the referenced PNGs (original bytes, verbatim — identical pixels), passes pre-packed atlases through as-is while still auto-repacking separate O/R/M maps into an ORM, writes the standard slots + `extras.vkfw_material`, and emits the slimmed scene. Re-runnable. Texture paths in a recipe are used **only at bake time**, but keep their on-disk case exact (several maps are `.PNG`) so re-baking works on Linux too.

**Size note.** These characters use 8K skin maps, so a baked GLB is large (~335–380 MB vs ~21 MB). This is inherent to the source textures (full-res verbatim embedding was chosen for pixel-identical results). `*.glb` under `resources/` is **Git-LFS tracked** (`.gitattributes`) — commit the baked GLBs through LFS, not as plain blobs. Because the maps are baked into the GLB, the **SLViewer distributable does not ship the loose `resources/textures/<char>/` folders** (they'd be ~1.2 GB of duplicated bytes) — see the `install(DIRECTORY … resources/textures … PATTERN "<char>" EXCLUDE)` rule in `CMakeLists.txt`.

**Key files:** `tools/bake_glb_material.py` (baker) + `tools/bake_recipes/*.json` (material source of truth), `ext/Vulkan-Engine/src/tools/loaders.cpp` (`load_GLB` + `GLBMaterialAux` + `load_PNG_from_memory`; `SetImagesAsIs` keeps the 8K maps raw and they're decoded on demand), `ext/Vulkan-Engine/include/engine/tools/loaders.h` (`GLBImage` / `GLBMaterialAux`), `src/scene_loader.cpp` (`GLBTexCtx`, `resolve_texture` `$GLB[...]` + channel resolution, `assemble_baked_materials`), and for packed atlases `ext/Vulkan-Engine/{include/engine/core/materials/physically_based.h,src/core/materials/physically_based.cpp}` (`set_*_channel` → `dataSlot10.y`) + `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` (`packedChannels`).

### Eyelash Shading Models (under evaluation)

Four competing eyelash lighting models live behind **one** pipeline, selected per
material by `EyelashMaterial::Variant` (scene JSON `"variant"`, 0–3). One shader
with a switch — not four shader files — so every model can be compared **in the
same frame under identical lighting**, and so a model can be flipped live in the
GUI without a rebuild. This is deliberately temporary: once a winner is picked the
losers get deleted and the selector collapses away.

| `variant` | Model | Where it lives |
|-----------|-------|----------------|
| 0 | **Baseline** — the original `evalEyelashBSDF` Marschner lobes | `epic_hair_BSDF.glsl` |
| 1 | **Matte fiber** — no R/TRT/env-sheen; wrapped fiber diffuse + forward scatter | `evalMatteLash` in `eyelash_strand.glsl` |
| 2 | **Tapered** — root→tip thinning (geometry stage) + transmission ramp (fragment stage) | `eyelash_strand.glsl`, both stages |
| 3 | **Coverage** — energy-conserving sub-pixel width via hashed MSAA sample masking | `eyelash_strand.glsl`, geometry stage (widening) + fragment stage (`gl_SampleMask`) |

Why variant 3 matters: at normal framing a `thickness: 0.001` fiber projects to
roughly **0.15 px**, so the rasterizer either drops it or draws it a full pixel wide
at full opacity — that quantization is a large part of the "shiny wire" read. The
model widens the quad to `min_pixel_width` and spends the inverse as partial MSAA
coverage, so the energy is preserved.

**It does NOT use fixed-function alpha-to-coverage, and must not.** Alpha-to-coverage
derives its sample pattern from the alpha *value* alone. Every fiber in a lash line
carries nearly the same coverage, so they all resolve onto the *same* subsample and
the lash mass never accumulates — at character framing that collapses the whole lash
to 1/8 intensity and the lashes visually vanish. (This is invisible in
`eyelash_lab.json`, whose groom is ~5× larger on screen; it only shows up on a real
character. `alphaToCoverage` is therefore explicitly `false` on the eyelash pass.)
Instead the fragment shader writes `gl_SampleMask[0]` itself, keeping a hashed subset
of `gl_SampleMaskIn` with stochastic rounding. The hash is keyed on the per-strand
random plus `gl_FragCoord` — deliberately not on time or world position, so the
pattern is stable frame to frame and the lashes don't shimmer under animation.
Naturally this needs MSAA to have samples to spend; at `"msaa": 1` there is nothing
to subdivide and the model degenerates to the baseline.

**Landed configuration (maria, 2026-07-22).** `variant: 3` + `min_pixel_width: 1.0`
+ `use_legacy_absorption: true` + **`TT_power: 0.25`** (down from 2.5). The scalp hair
(`maria_hair`) also got `use_legacy_absorption: true`.

**Why the eyelash shader cannot just be the epic hair shader with a different TT value.**
`evalEpicHairBSDF` has **no `Rpower`/`TTpower`/`TRTpower` factors and no `backlit` term at
all** — its lobes are `Mp * Np * Fp * Tp * grazingTerm`, full stop. So a `hairepic`
material would *silently ignore* `TT_power`, which is the single knob the eyelash tuning
now depends on. Exposing those three powers plus `use_backlit` is the entire reason
`evalEyelashBSDF` exists. On top of that the eyelash path carries the variant selector,
and `variant: 3`'s sub-pixel coverage (quad widening + hashed `gl_SampleMask`) has no
equivalent in the epic path — that is what gives the lash line its density. Both are load
bearing; the two shaders cannot be collapsed as things stand.

`use_legacy_absorption`, by contrast, **is** shared: it is a plain `hairepic` field on the
common `EpicHairBSDF`, so it applies to scalp hair, brows and lashes alike. It is the only
easy lever against the white TT blowout on *scalp* hair, since `TT_power` does nothing there.

**Uniform packing.** The eyelash block rides in `dataSlot9` (`variant`,
`sheen_scale`, `tip_taper`, `min_pixel_width`) via an `EyelashMaterial::get_uniforms()`
override — epic hair only uses slots 1–8, so there is no UBO layout change. **Gotcha:**
`MaterialUniforms` is consumed by hand-written GLSL blocks with no reflection, and
the hair block ends `float variability; vec3 tintColor;`. Under std140 that `vec3`
aligns up to offset 128 while the CPU packs the tint at 116 — so anything declared
after it lands a slot late. `eyelash_strand.glsl` therefore declares the tint as
three separate floats. `tintColor` is unused by both hair shaders, so the epic path
was never affected.

**Directional lights and the hair shaders (gotcha).** `DirectionalLight::get_uniforms`
packs the light **direction** (view space, `w = 0`) into `LightUniform::position`,
and `m_direction` points **toward** the light — that is how `physically_based.glsl`
consumes it (`wi = normalize(position.xyz)` under `type == DIRECTIONAL_LIGHT`).
Any shader deriving `L` must branch on the type; treating `position` as a point
puts the light one unit from the view origin, collapsing `L` onto `V`. Both hair
shaders had this bug (fixed 2026-07-22) and now carry the same branch, plus a
`w = 0` transform for the `computeHairShadowCone` direction. While it was broken,
`inBacklit` was pinned at ~0 under any directional key, which **silently disabled
the entire TT lobe** — the dominant lobe on a dark backlit fiber.

**The comparison scenes.** Two, and the distinction matters:

- **`resources/scenes/eyelash_solo.json` — the one to trust.** A single groom
  centred on a `lab_card.obj` backdrop. Compare variants by rendering it repeatedly
  with only the material changed, so every render is the *same* groom at the *same*
  screen position under the *same* light. This is the only fair comparison (see the
  bias note below).
- **`resources/scenes/eyelash_lab.json` — a quick simultaneous eyeball, not a
  measurement.** Five copies of `models/maria/eyelashes.hair` in a 2-column ×
  3-row grid (V0 top-left, V1 top-right, V2 mid-left, V3 mid-right, V4 bottom-left),
  each a different variant.

Notes that apply to both:
- The key is a **far directional light**. For a directional light only `direction`
  affects shading — `position` merely places the dummy and the shadow view — so the
  key is aimed as a 3/4 back rim, deliberately: a front key drives `inBacklit` to 0
  and gates TT off, hiding the very thing being compared. The cards stay readable
  because the IBL lights them, not the key.
- The backdrop card means lashes are judged both in silhouette (in the gaps) and
  against a surface — an isolated groom is backlit in every pixel, which biases tuning.
- **The engine maps world −X to screen-right** (hence the 180° Y rotation on every
  character), so the grid's X positions are mirrored to match the V0…V4 naming.
- **A groom's vertices sit ~33 units from its object origin**, so the model matrix's
  scale term also *translates* it. Every cell must stay at `scale: 0.55` or the grid
  flies apart — this is not a normal "make it bigger" knob.

⚠️ **The grid has a strong vertical positional bias — do not read TT across cells.**
With *identical* material in all five cells, the top row renders ~3.2× the lash
contrast of the bottom row, and under a `TT_power` boost only the top row develops a
highlight at all (the rows below stay at the card value). Verified reproducible with
the mesh declaration order reversed, so it follows **position**, not draw order.
Disabling `adv_shadows` barely changes it, so it is *not* the shared hair voxel volume,
and a long lens (which cuts the `dot(-L,V)` spread to ~10%) does not fix it either —
**the mechanism is still unexplained**. Also note run-to-run renders of this scene are
**not deterministic**: repeating the same render gives ~1.0 mean abs diff per cell, so
small numeric comparisons here are meaningless.

### Hair-to-Scalp Surface Binding

Strand hair (`.hair` — scalp hair, eyebrows, eyelashes) can be bound to a character head's surface so it sits on the skin (no clip / no float), follows the head's **morph + skeletal** animation, and keeps each strand's silhouette. Neural `.ply` hair is out of scope (deprecated).

**How it works.** Each strand root is projected onto the nearest head triangle (rest pose); per strand we store the triangle, the barycentric coords, and every strand vertex expressed in the bind-pose root frame. Each frame the frame is rebuilt from the head's *deformed* surface vertices and the rigid delta is applied to the whole strand (rigid-per-strand → silhouette preserved). The head's deformed vertices are produced CPU-side every frame by `Geometry::apply_deformation`, so binding follows both morphs and skinning. On bind the hair is reparented under the head with an identity local transform (its model matrix equals the head's); all deformation arrives through the surface, not a joint attachment.

**Interactive bind mode (HairViewer "HAIR BINDING" panel):**
1. Pick the hair mesh in the dropdown (head = first morph/skinned mesh, auto-detected).
2. Seat the hair with the **Position / Rotation / Scale** sliders (live preview). Assets are usually grossly misaligned, so this gross alignment is required before binding.
3. **Normal offset** lifts roots along the surface normal (0 = on the skin).
4. **Declip to scalp** (default on) clamps strand vertices to the root's scalp plane so bodies don't sink into a head fatter than the groom (cheap, baked at bind time; accurate for short hairs, approximate for long drapes).
5. **Bind** projects + snaps roots and reparents the hair. Re-bind freely; the console prints a report (`root->surface dist`, `snap`).
6. **Save** writes a sidecar next to the asset (`<hair file>.hbnd`); **Load** re-applies it.

**Sidecar (`.hbnd`).** Binary: header (`HBND`, version, strand/vertex counts, normal offset) + per-strand (triangle indices + barycentric) + per-vertex (local position + tangent). Validated against the hair geometry on load (counts must match) and auto-loaded at startup when present.

**Scene JSON** (a hair mesh entry; see @SCENE.md):
- `"bind_to": "<head mesh name>"` — bind onto that mesh's surface (replaces `attach_to` for bound hair).
- `"binding": "<path.hbnd>"` — optional sidecar path (relative to resources); defaults to `<hair file>.hbnd`.

When no mesh declares `bind_to`, both viewers auto-discover the head (first morph/skinned mesh) and bind every `.hair` mesh, loading each `<hair file>.hbnd` if present. The scene loader only records the request (`LoadResult::hairBindings`); each application builds the `HairBinder`. Both **HairViewer** (`src/application.cpp`) and **SLViewer** (`src/slviewer/application_sl.cpp`) run the same `setup_hair_binding()` + per-frame `binder->update()` logic, so `hair_binding.cpp` is linked into both targets (added to `SLVIEWER_SOURCES` in `CMakeLists.txt`). Without this the strand hair renders at its raw unbound groom position (off-frame).

**Engine notes / limitations.**
- `.hair` geometry is marked animatable (CPU-writable VBO) in `load_hair`, which excludes it from the RT BLAS (the forward hair path doesn't use it).
- **Animatable geometry keeps two ring buffers, not one.** Besides the deformed-vertex VBO ring, `Geometry::cycle_animatable_upload` also rings the **position SSBO** (`vao.posSSBO`, one `Vec4`/vertex), because the hair voxelization (`HAIR_VOXELIZATION_PASS`, `OPTICAL_DENSITY` mode), SSAO and SSR read strand positions from that bindless buffer — not the VBO. The bindless descriptor is re-pointed at the live region each frame (`forward_pass`/`hair_voxelization_pass` `update_uniforms` pass `readOffset = posFrameOffset`). Without this the hair's volumetric self-shadow/scattering freezes at the groom pose while the visible strands move (the strand model matrix is ~identity — animation is baked into the vertices). `RING == 3` for both rings (DOUBLE buffering; bump to 4 for TRIPLE).
- Per-frame reconstruction is CPU-side (mirrors `apply_deformation`); fine for moderate strand counts (GPU compute path is possible future work). A static (non-animated) head reconstructs once.
- Declip is a tangent-plane clamp — long strands far from their root may still clip (full surface-collision declip is future work). SLViewer headless export now drives the binders too (same `setup_hair_binding()` + per-frame `binder->update()` as HairViewer), so exported video matches the interactive view.
- **SLViewer's "bald first frame" (fixed 2026-07-27).** The first captured frame used to have no surface-bound strand hair (bald: no scalp hair, brows or lashes) while frames 1+ were correct. Root cause: geometry is uploaded to the GPU *lazily* on the first `render()` (`ResourceManager::upload_geometry_data` seeds animatable-VBO ring region 0 with the raw groom vertices, then flips `loadedOnGPU=true`). On a cold frame 0 the binder's `update()` ran *before* that upload, so `Geometry::upload_vertices` no-oped (`!loadedOnGPU`) and the hair drew its seeded groom region — off-frame = bald. Fix: `SLApplication::init()` now does one **throwaway warm-up `render(m_scene)`** after `setup()` and *before* the capture callback is registered (so nothing is written to disk — `render()` guards a null pre-submit callback, and it neither advances the animation nor the frame counter). That forces the lazy upload, so the first *captured* frame's `binder->update()` actually writes the bound strands. Verified by A/B: with the warm-up off, `mean|frame0−frame1|` ≈ 1.5 (≈7× the ~0.2 run-to-run noise floor); with it on, ≈ 0.2 (noise-level). `frame_00000.png` is now safe to read.

**Key files:** `src/hair_binding.{h,cpp}` (`HairBinder`: bind / update / sidecar IO), `src/gui.{h,cpp}` (`HairBindWidget`), `src/application.cpp` (`setup_hair_binding()` + per-frame `binder->update()`), `src/scene_loader.{h,cpp}` (`bind_to`/`binding` → `LoadResult::hairBindings`), and engine hooks `Geometry::{get_deformed_vertices,get_strand_offsets,set_animatable,upload_vertices,update_bounds}` + strand-offset capture in `Tools::Loaders::load_hair`.

### Animation format

The animations loaded are in json format. The specifics of this format and its structure are defined in detail in @ANIMATION.md

### Scene format

Scenes are defined in JSON and loaded at runtime by `src/scene_loader.{h,cpp}`. Schema, material types, light types, animation binding rules, and worked examples live in @SCENE.md. Known-good character scenes: `alex`, `javi`, `maria`, `nadia`, `neural_tono`, `bust_strands`. The default scene (used by SLViewer when `--scene` is omitted, and by HairViewer) is `resources/scenes/maria.json`.

> **Note:** the old `resources/scenes/default.json` (Alex + `haircard`/OBJ hair) was **removed** — it segfaulted on the haircard hair path. `maria.json` is the default now.

### Scene Selection

Scenes are JSON-driven (`resources/scenes/*.json` — see @SCENE.md).

- **HairViewer** loads a scene unconditionally (currently `resources/scenes/nadia.json`). To use a different scene, either edit that file or change the path in `src/application.cpp::HairViewer::setup()`.
- **SLViewer** accepts an optional `--scene <path>` flag; when omitted it falls back to `resources/scenes/maria.json`.

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
├── SLViewer              # ELF binary (shaders baked in as SPIR-V)
├── ffmpeg                # bundled static ffmpeg GPL build (BtbN linux64)
├── THIRD_PARTY_NOTICES.txt
└── resources/
    ├── meshes/           # engine built-in meshes (sphere, cube)
    ├── scenes/           # *.json (maria is the default; no default.json)
    ├── models/<char>/    # <char>.glb (materials + skin/eye maps baked in) + .hair strands
    ├── textures/         # global HDRIs, scatterDistance/ SSS LUTs, skin detail maps, icon
    │                     #   NOTE: per-character folders (alex/ javi/ maria/ nadia/) are
    │                     #   NOT shipped — those maps are baked verbatim into each <char>.glb.
    └── animations/       # test_anim.json, test_morph.json, ...
```

No `resources/shaders/` is shipped — shaders are pre-compiled to SPIR-V at
build time and linked into the binary. See **Embedded SPIR-V for SLViewer**
below.

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
├── SLViewer.exe          # /MT static MSVC runtime — no VC++ redist. Shaders baked in as SPIR-V.
├── ffmpeg.exe            # bundled static ffmpeg GPL build (BtbN win64)
├── vulkan-1.dll          # Vulkan loader (from %VULKAN_SDK%\Bin\, or System32 fallback)
├── shaderc_shared.dll    # Shaderc — a load-time import even though SLViewer never calls it
├── THIRD_PARTY_NOTICES.txt
└── resources\            # same tree as Linux — no shaders\
```

`vulkan-1.dll` and `shaderc_shared.dll` are both located automatically at configure time and bundled next to the exe. If either is missing, CMake emits a warning and the DLL must be copied manually. The MSVC runtime is compiled in statically (`/MT`), so no VC++ Redistributable is required on the target machine.

**Why `shaderc_shared.dll` is bundled even though SLViewer uses baked SPIR-V.** The engine is a static lib that links the Shaderc **import** lib (`shaderc_shared`) on Windows, because the SDK's static `shaderc_combined.lib` is built `/MD` and won't link into this `/MT` build (unresolved `__imp_exp2` etc.). So *every* exe linking the engine — SLViewer included — carries a **load-time** dependency on `shaderc_shared.dll` and won't launch without it, even though SLViewer never actually calls Shaderc (HairViewer does, at runtime). Target machines have no Vulkan SDK, so the DLL is shipped. Linux is unaffected: it links the static `shaderc_combined.a`, so there's no runtime DLL. To drop the dependency entirely, a `/MT`-built static Shaderc would be needed (the SDK doesn't provide one).

**`vulkan-1.dll` source.** Preferred from `%VULKAN_SDK%\Bin\`, but recent SDKs (1.4.x) no longer ship the loader there — it's installed to `System32` by the runtime installer / GPU driver, so the CMake rule falls back to `%WINDIR%\System32\vulkan-1.dll`. Bundling the Windows loader is safe because it discovers ICDs via the registry, not SDK-relative paths (this is why Linux deliberately does *not* bundle `libvulkan.so.1`). Even unbundled, most target machines resolve `vulkan-1.dll` from `System32` via the default DLL search path — which is why a missing loader is rarely the first error seen, but a missing `shaderc_shared.dll` (present nowhere but the SDK) always is.

**Note**: The ffmpeg `.zip` for Windows is downloaded at configure time (same as Linux). If the download fails, system `ffmpeg` on `PATH` is used as a fallback at runtime.

### Embedded SPIR-V for SLViewer

SLViewer ships with shaders **baked into the binary** as SPIR-V, so no GLSL
source leaves the build tree in the distributable. HairViewer is unchanged —
it still reads `.glsl` from disk and compiles via Shaderc at runtime, which
keeps shader iteration fast for dev work.

**Build pipeline** (driven by `CMakeLists.txt` section 5b):
1. `tools/precompile_shaders.py` walks `ext/Vulkan-Engine/resources/shaders/`,
   mirrors `ShaderSource::read_file`'s splitter (unified `#shader` directives
   with non-recursive `#include` from `scripts/`).
2. Each stage is compiled with `glslc` (Vulkan 1.3 / SPIR-V 1.4 to match the
   runtime Shaderc config), then run through `spirv-opt --strip-debug` so
   reconstructed GLSL loses your original identifiers.
3. The script emits `build/generated/embedded_shaders.cpp` — a registry of
   `(path, stage, uint32_t[])` entries plus a static `AutoRegister`
   constructor that calls `VKFW::set_embedded_shader_registry()`.
4. That `.cpp` is linked **only** into the SLViewer target.

**Runtime dispatch** (`shaderpass.cpp`):
- `GraphicShaderPass::build_shader_stages` / `ComputeShaderPass::build_shader_stages`
  call `VKFW::has_embedded_shader_registry()` first.
- If a registry is installed (SLViewer), look up the SPIR-V by `(filePath, stage)`
  and feed it straight to `vkCreateShaderModule` — `read_file` and Shaderc are
  skipped. A missing entry **hard-fails** with `"[Shader] No embedded SPIR-V for: ..."`.
- If no registry is installed (HairViewer), the existing GLSL-on-disk +
  Shaderc path runs unchanged.

**Key files**:
- `tools/precompile_shaders.py` — codegen
- `ext/Vulkan-Engine/include/engine/core/shader_registry.h` — registry API
- `ext/Vulkan-Engine/src/core/shader_registry.cpp` — registry storage + lookup
- `ext/Vulkan-Engine/src/graphics/shaderpass.cpp` — runtime branch

**Adding / editing a shader**: just edit the `.glsl` under
`ext/Vulkan-Engine/resources/shaders/`. The custom command in `CMakeLists.txt`
re-runs the precompile script whenever any `.glsl` changes, so the embedded
blobs stay in sync. HairViewer picks up the change at next launch (no rebuild
needed); SLViewer picks it up at next build.

**Skipped shaders**: shaders with missing includes (deferred-renderer dead
code) or broken signatures are warned and skipped during codegen. If SLViewer
ever requests one, the hard-fail at startup names the file clearly.

## SLViewer — Headless Video Export

`SLViewer` renders a scene from a JSON animation file and encodes the result as an MP4 using ffmpeg. When `--scene` is omitted it uses `resources/scenes/maria.json`.

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
| `--scene <file.json>` | `resources/scenes/maria.json` | Scene JSON. Schema in @SCENE.md. |
| `--output <file.mp4>` | `output.mp4` | Output video path |
| `--width N` | 1920 | Render width in pixels |
| `--height N` | 1080 | Render height in pixels |
| `--msaa 1\|4\|8` | `8` | Hardware MSAA sample count. 8× is needed for clean hair fibers; 1× is much faster but heavily aliased. |
| `--keep-frames` | off | Retain the per-frame PNG dump in the temp dir after encoding |
| `--log-level error\|warn\|verbose` | `warn` | Vulkan validation message severity filter |

### Example

```bash
./SLViewer ../resources/animations/test_anim.json \
           --scene ../resources/scenes/maria.json \
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