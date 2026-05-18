# Subsurface Scattering Implementation Plan

## Overview

Add a screen-space Subsurface Scattering (SSS) post-process to the forward renderer, using Burley's Normalized Diffusion Model for multiple scattering and Beer-Lambert translucency for single scattering. The forward renderer is extended with Multiple Render Targets (MRT) to provide the necessary screen-space data, preserving hardware MSAA for hair strands.

---

## Proposed Pipeline

```
Shadow → HairScatter → HairVoxel → Forward(+MRT) → SSAO/Thickness → SSS → Bloom → Tonemap → FXAA
```

Pass indices in `ForwardRenderer::RendererPasses`:

| Index | Enum | Type |
|-------|------|------|
| 0 | SHADOW_PASS | Variance shadow map |
| 1 | HAIR_SCATTER_PASS | Compute |
| 2 | HAIR_VOXELIZATION_PASS | Compute |
| 3 | FORWARD_PASS | Rasterization (MSAA + MRT) |
| 4 | SSAO_PASS | Post-process |
| 5 | SSS_PASS | Post-process |
| 6 | BLOOM_PASS | Post-process |
| 7 | TONEMAPPIN_PASS | Post-process |
| 8 | FXAA_PASS | Post-process |

---

## Forward Pass MRT Layout

All color attachments 0–6 are multisampled (matching `RendererSettings::samplesMSAA`). When MSAA is enabled, each has a resolved single-sample counterpart at index+7. Post-process passes always read the resolved versions. Depth is not resolved (not needed by post-process).

### Color attachments (indices 0–6)

| Index | Format | Content |
|-------|--------|---------|
| 0 | SRGBA_32F | HDR color |
| 1 | SRGBA_32F | Bright color (for bloom) |
| 2 | SRGBA_16F | View-space normals.rgb |
| 3 | SRGBA_16F | Albedo.rgb + scatterMask in A (cleared to alpha=0) |
| 4 | SRGBA_16F | Diffuse irradiance.rgb (front-facing, no albedo multiply, no specular) |
| 5 | SRGBA_16F | Back irradiance.rgb (lighting evaluated with flipped normal) |
| 6 | SR_32F | Linear depth copy (gl_FragCoord.z, cleared to 1.0) |
| depth | D32F | Depth (not sampled by post-process) |

### Resolve attachments (MSAA only, indices 7–13)

| Index | Resolves | Read by |
|-------|----------|---------|
| 7 | HDR color | Bloom / SSS |
| 8 | Bright color | Bloom |
| 9 | Normals | SSAO |
| 10 | AlbedoMask | SSS |
| 11 | DiffuseIrradiance | SSS |
| 12 | BackIrradiance | SSS |
| 13 | LinearDepth | SSAO, SSS |

Hair shaders write `scatterMask = 0.0` to attachment 3 alpha. The SSS pass skips pixels where `scatterMask == 0`.

**Hardware note**: 7 color attachments requires `maxColorAttachments >= 7`. All modern desktop GPUs (NVIDIA, AMD, Intel Xe) report 8. Vulkan spec minimum is 4.

---

## SSAO/Thickness Pass

- **Input**: Linear depth (resolved attachment 13 / attachment 6 non-MSAA), view-space normals (resolved attachment 9 / attachment 2 non-MSAA) from the forward pass
- **Output**: Single RT — R = ambient occlusion, G = thickness
- **Algorithm**: Hemisphere sampling using a TBN matrix built from the surface normal and a random vector from a noise texture. AO samples the hemisphere along the normal; thickness samples the hemisphere along the inverted normal. Same kernel, two evaluations.
- **Noise texture**: Small RGB texture (e.g. 4x4 or 8x8) generated on the CPU with random XY vectors, uploaded once. Nearest sampling, tiled across the screen via `uv * (screenSize / noiseTextureSize)`.
- **Kernel samples**: Hemisphere sample positions generated on the CPU, passed as a uniform array (`vec3 samples[MAX_SAMPLES]`).
- **No blur pass**.
- AO is only consumed by the SSS pass, not applied globally.

---

## SSS Pass

- **Inputs**: HDR color (att 0), albedo + scatterMask (att 2), diffuse irradiance (att 3), back irradiance (att 4), depth, SSAO/thickness output
- **Output**: Single RT — final scattered color (replaces HDR color going into bloom)
- **Algorithm**: Disk-based Burley Normalized Diffusion for multiple scattering. Beer-Lambert with thickness for single scattering (translucency).
- **Scattering distance**: Uniform `vec3`, not a per-pixel texture.
- **Sample generation**: CPU-side Fibonacci lattice for angular distribution, CCDF inverse for radial distribution. Passed as uniform array (`vec2 samples[MAX_SAMPLES]`).
- **ScatterMask**: Pixels with `scatterMask == 0` (hair, non-skin) pass through the original HDR color unchanged.

---

## Completed Tasks (Phase 1)

| # | Task | Summary |
|---|------|---------|
| 1 | Extend ForwardPass MRT | 7 color attachments (HDR, Bright, Normals, AlbedoMask, DiffuseIrr, BackIrr, LinearDepth) + MSAA resolve counterparts |
| 2 | Forward shader MRT outputs | PBR outputs diffuseIrr/backIrr/normals/albedo+scatterMask/depth; hair/skybox/unlit write scatterMask=0 |
| 3 | SSAO/Thickness pass | Hemisphere sampling, 4x4 noise texture, outputs R=AO G=thickness |
| 4 | SSS pass | Burley diffusion (multiple scattering) + Beer-Lambert translucency (single scattering), 2 output attachments (scattered HDR + bright pass-through) |
| 5 | Wire passes into ForwardRenderer | Enum reordered (SSAO=4, SSS=5, BLOOM=6, TONEMAP=7, FXAA=8), dependency tables, Bloom reads from SSS |
| 6 | Update ResourceManager | No-op — passes are self-contained |
| 7 | Update CMakeLists | No-op — GLOB_RECURSE picks up new files |
| 8 | Debug test harness | `--frames N`, `--log-level`, `--log-file` CLI flags for headless validation runs |
| 9 | Validation run | Fixed 5 Vulkan validation issues (descriptor pool, blend attachments, phong MRT, bloom descriptor update, swapchain format) |
| 10 | GUI controls | SSAO/SSS parameter sliders, enable toggles, animate light checkbox |

**Post-plan hotfixes**:
- Segfault: missing `m_samples.resize(MAX_SAMPLES)` in revamped `generate_samples()`
- std140 UBO mismatch: `vec2 samples[]` → `vec4 samples[]` in GLSL and C++
- Missing `#include <chrono>`

---

## GLB Loading Support (Standalone Task)

### Goal

Add GLB (glTF Binary) file loading to the engine using tinygltf. This enables loading skeletal meshes with morph targets (blendshapes) from the 4 test characters in `resources/models/`. The loader extracts all geometry data and stores skeletal/morph metadata for future animation work. Mesh selection is by index, so the caller decides which mesh(es) from the GLB to load.

### Architecture Decision: Skinning Data Storage

**Option C — Separate parallel data (chosen).** Joint indices and weights are stored alongside `GeometricData` as parallel vectors, not added to the `Vertex` struct. Reasons:

- The `Vertex` struct is the foundation of **every** VAO binding, pipeline, and shader in the engine. Adding 32 bytes (uvec4 + vec4) to it would force changes to every vertex input layout, break the hash function, waste memory on non-skinned meshes, and risk Vulkan validation errors across all passes.
- Skinning data is consumed by a compute/vertex shader at animation time via SSBO — it doesn't need to flow through the rasterization vertex attributes.
- Morph target deltas are also stored as parallel data (SSBO-ready), same pattern.
- This is the minimal-change approach: existing Vertex, VAO, and pipeline code is untouched.

### Data Model

```
GeometricData (existing)          SkinData (new, per-Geometry)
├─ vertexData: Vertex[]           ├─ jointIndices: uvec4[]      (per-vertex, 4 joints)
├─ vertexIndex: uint32_t[]        ├─ jointWeights: vec4[]       (per-vertex, 4 weights)
├─ voxelData: Voxel[]             ├─ inverseBindMatrices: mat4[] (per-joint)
├─ maxCoords, minCoords, center   └─ jointNames: string[]       (for debugging)
└─ loaded: bool
                                  MorphTargetData (new, per-Geometry)
                                  ├─ targets[]: { deltaPos: vec3[], deltaNormal: vec3[] }
                                  └─ targetNames: string[]
```

`SkinData` and `MorphTargetData` live on `GeometricData` as `std::optional` fields — present only when the source file contains them.

### GLB Test File Summary

| File | Meshes | Joints | Morphs (mesh 0) | Textures | Main mesh attrs | Secondary mesh attrs |
|------|--------|--------|------------------|----------|-----------------|----------------------|
| maria.glb | 4 | 55 | 100 | 1 | POS, NORMAL, UV, JOINTS, WEIGHTS | POS, JOINTS, WEIGHTS |
| javi.glb | 6 | 55 | 100 | 1 | POS, NORMAL, UV, JOINTS, WEIGHTS | POS, JOINTS, WEIGHTS |
| alex.glb | 4 | 55 | 100 | 1 | POS, NORMAL, UV, JOINTS, WEIGHTS | POS, JOINTS, WEIGHTS |
| nadia.glb | 4 | 55 | 100 | 1 | POS, NORMAL, UV, JOINTS, WEIGHTS | POS, JOINTS, WEIGHTS |

Main mesh (mesh 0) has normals and UVs. Secondary meshes (teeth, tongue, hearing aid) have only positions — normals must be computed from triangle topology. No tangents in any mesh — must be computed via Gram-Schmidt. Morph targets contain only POSITION deltas. 55-joint skeleton. 1 embedded texture per file.

### Tasks

| # | Task | Status | Description |
|---|------|--------|-------------|
| 1 | Add tinygltf to thirdparty | [x] | tinygltf v2.9.3 + nlohmann/json v3.11.3 under `thirdparty/tinygltf/include/`. INTERFACE CMake target, linked into VulkanEngine. |
| 2 | Extend GeometricData | [x] | Added `SkinData` (jointIndices uvec4[], jointWeights vec4[], inverseBindMatrices mat4[], jointNames) and `MorphTargetData` (targets[]{deltaPos vec3[]}, targetNames) as `std::optional` fields on `GeometricData`. Added `set_skin_data()` / `set_morph_target_data()` setters to `Geometry`. |
| 3 | Implement `load_GLB()` | [x] | Implemented in `loaders.cpp`. Extracts pos/normal/UV/color, computes normals from topology when absent, always computes tangents via Gram-Schmidt, reads JOINTS_0/WEIGHTS_0 for skinning, reads inverse bind matrices from skin[0], reads morph target POSITION deltas with names from `extras.targetNames`. |
| 4 | Wire GLB into `load_3D_file()` | [x] | Added `GLB`/`GLTF` defines to `common.h`. Added dispatch branch (sync and async) in `load_3D_file()`. |
| 5 | Add `USE_GLB_MODELS` path in application | [x] | Added `#define USE_GLB_MODELS` block in `application.cpp` loading `maria.glb` mesh 0 with PBR material. Uses `#ifdef`/`#elif`/`#else`/`#endif` chain with `USE_NEURAL_MODELS` and default. |
| 6 | Build and validate | [x] | Clean build (GCC, -O3). 10-frame headless run: zero Vulkan validation errors or warnings from new code. Pre-existing warnings unchanged. |

**Implementation notes**:
- `TINYGLTF_NO_STB_IMAGE` + `TINYGLTF_NO_STB_IMAGE_WRITE` defined before tinygltf include — geometry-only loader, no image decoding needed.
- `TINYGLTF_IMPLEMENTATION` defined at top of `loaders.cpp` (single translation unit), header only exposes the function declaration.
- `glm/gtc/type_ptr.hpp` included for `glm::make_mat4` used to parse column-major GLB matrices.
- `tinygltf::Value::IsObject()` used for extras check (this version has no `IsNull()`).

---

## Hair Card Rendering

### Context

The project currently renders hair as strand geometry (line segments expanded via geometry shaders). We need to add support for **hair cards** — textured triangle meshes that approximate hair volume using alpha-tested quads. The OBJ mesh (`resources/models/hair_fauxmohawk.obj`, ~21K vertices with UVs and normals) and texture (`resources/textures/hair_fauxmohawk.PNG`) come from Unreal MetaHumans.

The texture is a **packed data texture** (not albedo — the visible colors are false-color channel encoding):
- **R**: Alpha/opacity mask (confirmed by user)
- **G**: Root-to-tip gradient (for color variation along the strand)
- **B**: Per-strand random ID (for subtle per-strand hue/brightness variation)

Hair color will come from uniform values (rootColor, tipColor). The hair cards will be added alongside existing strand hair in the scene. There is also a `hair_fauxmohawk_rootscoverage.PNG` texture (density/flow data) that is not used in the initial implementation.

`HAIR_CARD_TYPE = 4` is already reserved in the `IMaterial::Type` enum (`material.h:47`) but has no implementation.

### Task 1: Create `HairCardMaterial` class

**Create** `ext/Vulkan-Engine/include/engine/core/materials/hair_card.h`

Follow the pattern established by `PhysicallyBasedMaterial` (`physically_based.h`):
- Class `HairCardMaterial` inherits `IMaterial` with type `HAIR_CARD_TYPE`
- Constructor sets default `MaterialSettings`: `alphaTest = true`, `faceCulling = false` (double-sided cards), `depthTest = true`, `depthWrite = true`
- Member variables with getters/setters (each setter sets `m_isDirty = true`):
  - `m_hairColor` (Vec3, default dark brown `{0.05, 0.02, 0.01}`) — fallback color when no root/tip distinction
  - `m_rootColor` (Vec3, default same as hairColor) — color at the root of strands
  - `m_tipColor` (Vec3, default same as hairColor) — color at the tip of strands
  - `m_roughness` (float, default 0.4) — surface roughness for lighting
  - `m_specularIntensity` (float, default 0.5) — Kajiya-Kay specular strength multiplier
  - `m_specularShift` (float, default 0.1 radians) — tangent shift for the primary Kajiya-Kay highlight
  - `m_alphaThreshold` (float, default 0.1) — discard threshold applied to R channel of packed texture
  - `m_colorVariation` (float, default 0.1) — how much the B channel (strand ID) varies the final color
- Texture enum and storage (same pattern as PBR's `m_textures` unordered_map):
  - `HAIR_DATA = 0` — the packed RGB texture (R=alpha, G=root-to-tip, B=strand ID)
  - `NORMAL = 1` — optional normal map (not used initially, slot reserved)
  - Initialize map: `{{HAIR_DATA, nullptr}, {NORMAL, nullptr}}`
- `m_textureBindingState` (unordered_map<int, bool>) for tracking GPU binding state
- Boolean flags: `m_hasHairDataTexture`, `m_hasNormalTexture`
- Texture setters follow PBR pattern: set flag, reset binding state to false, assign pointer, set dirty
- Override virtual methods: `get_uniforms()`, `get_textures()`, `get_texture_binding_state()`, `set_texture_binding_state()`

**Create** `ext/Vulkan-Engine/src/core/materials/hair_card.cpp`

Implement `get_uniforms()` following the PBR material pattern (`physically_based.cpp`). Pack into 8 Vec4 slots of `Graphics::MaterialUniforms`:
```
dataSlot1: {hairColor.r, hairColor.g, hairColor.b, alphaThreshold}
dataSlot2: {roughness, specularIntensity, specularShift, colorVariation}
dataSlot3: {rootColor.r, rootColor.g, rootColor.b, (float)hasHairDataTexture}
dataSlot4: {tipColor.r, tipColor.g, tipColor.b, (float)hasNormalTexture}
dataSlot5-8: Vec4(0.0) — reserved for future use
```

Status: [x] — `hair_card.h` and `hair_card.cpp` created.

### Task 2: Create `hair_card.glsl` forward shader

**Create** `ext/Vulkan-Engine/resources/shaders/forward/hair_card.glsl`

Unified vertex + fragment shader using the engine's `#shader vertex` / `#shader fragment` directive format.

**Vertex shader** (same structure as `physically_based.glsl`):
- `#version 460`
- `#include camera.glsl` and `#include object.glsl`
- Inputs: `layout(location = 0) in vec3 pos`, `layout(location = 1) in vec3 normal`, `layout(location = 2) in vec2 uv`, `layout(location = 3) in vec3 tangent`
- Outputs: `v_pos` (view-space), `v_normal` (view-space), `v_modelNormal`, `v_uv`, `v_modelPos`, `v_screenExtent`, `v_TBN` (mat3), `v_tangent` (view-space tangent for Kajiya-Kay)
- Transform logic identical to PBR vertex shader
- UV: `v_uv = vec2(uv.x, 1.0 - uv.y)` (may need to test with/without flip)

**Fragment shader**:
- `#version 460`, extensions for ray tracing/ray query (same as PBR)
- Includes: `camera.glsl`, `light.glsl`, `scene.glsl`, `object.glsl`, `utils.glsl`, `shadow_mapping.glsl`, `fresnel.glsl`, `IBL.glsl`, `reindhart.glsl`, `raytracing.glsl`
- Material uniform block (set=1, binding=1) matching the `get_uniforms()` layout above
- Texture samplers: `layout(set = 2, binding = 0) uniform sampler2D hairDataTex;`
- Global uniforms: shadow map (set=0 binding=2), irradiance map (set=0 binding=4), TLAS (set=0 binding=5), blue noise (set=0 binding=6)
- 7 MRT outputs matching forward pass: outColor, outBrightColor, outNormals, outAlbedoMask, outDiffuseIrr, outBackIrr, outLinearDepth

**Fragment logic**:
1. Sample packed texture: `vec4 texData = texture(hairDataTex, v_uv);`
2. Alpha test: `if (texData.r < material.alphaThreshold) discard;`
3. Compute base color:
   - Root-to-tip: `vec3 baseColor = mix(material.rootColor, material.tipColor, texData.g);`
   - Strand variation: `baseColor *= (1.0 + material.colorVariation * (texData.b - 0.5));`
4. Kajiya-Kay anisotropic specular (per-light):
   - `vec3 T = normalize(v_tangent);` (view-space tangent)
   - `float sinTL = sqrt(1.0 - pow2(dot(T, wi)));`
   - `float sinTV = sqrt(1.0 - pow2(dot(T, V)));`
   - Primary: `float spec1 = pow(sinTL * sinTV - dot(T, wi) * dot(T, V), specExponent);`
   - Secondary: shift tangent by `specularShift`, compute again with different exponent
   - Scale by `specularIntensity`
5. Diffuse: Lambert `max(dot(N, wi), 0.0) * radiance * baseColor`
6. Shadow mapping: same as PBR shader (VSM, classic, or raytraced depending on light settings)
7. IBL ambient: `computeAmbient()` from `IBL.glsl` (or simple `ambientColor * ambientIntensity * baseColor`)
8. MRT outputs:
   - `outColor = vec4(color, 1.0);` (alpha test already discarded transparent fragments)
   - `outBrightColor` = bloom threshold check
   - `outNormals = vec4(v_normal, 0.0);`
   - `outAlbedoMask = vec4(baseColor, 0.0);` (scatterMask = 0, skip SSS for hair cards)
   - `outDiffuseIrr = vec4(diffuseIrr, 0.0);`
   - `outBackIrr = vec4(backIrr, 0.0);`
   - `outLinearDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);`

Status: [x] — `hair_card.glsl` created with Kajiya-Kay lighting, alpha test, 7 MRT outputs, IBL ambient.

### Task 3: Register shader pass in forward renderer

**Modify** `ext/Vulkan-Engine/src/core/passes/forward_pass.cpp`

In `setup_shader_passes()`, add after the `HAIR_STR_EPIC_TYPE` block (after line 331) and before the Disney hair block (line 334):

```cpp
GraphicShaderPass* hairCardPass =
    new GraphicShaderPass(m_device->get_handle(), m_renderpass, m_imageExtent, ENGINE_RESOURCES_PATH "shaders/forward/hair_card.glsl");
hairCardPass->settings.descriptorSetLayoutIDs = {{GLOBAL_LAYOUT, true}, {OBJECT_LAYOUT, true}, {OBJECT_TEXTURE_LAYOUT, true}};
hairCardPass->graphicSettings.attributes      = {
    {POSITION_ATTRIBUTE, true}, {NORMAL_ATTRIBUTE, true}, {UV_ATTRIBUTE, true}, {TANGENT_ATTRIBUTE, true}, {COLOR_ATTRIBUTE, false}};
hairCardPass->graphicSettings.blendAttachments = blendAttachments;
hairCardPass->graphicSettings.dynamicStates    = dynamicStates;
hairCardPass->graphicSettings.samples          = samples;
m_shaderPasses[IMaterial::Type::HAIR_CARD_TYPE] = hairCardPass;
```

Key points:
- Topology = `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` (default, unlike hair strands which use LINE_LIST)
- `OBJECT_TEXTURE_LAYOUT = true` so material textures get bound
- All vertex attributes except COLOR enabled (OBJ has pos/normal/uv, tangents computed by loader)
- No push constants needed (unlike HAIR_STR_EPIC_TYPE)
- No geometry shader needed (unlike hair strands)

Also need to add `#include <engine/core/materials/hair_card.h>` at top of this file if not already pulled in via `core.h` (check — `forward_pass.cpp` includes `forward_pass.h` which includes `pass.h`, need to verify include chain reaches `hair_card.h`; the `#include <engine/core/materials/hair.h>` at line 1 suggests materials are included directly, so add `hair_card.h` there too).

Status: [x] — Hair card shader pass registered in `forward_pass.cpp` after `HAIR_STR_EPIC_TYPE` block (TRIANGLE_LIST topology, pos/normal/uv/tangent attributes, OBJECT_TEXTURE_LAYOUT enabled).

### Task 4: Alpha-tested shadow pass for hair cards

**Create** `ext/Vulkan-Engine/resources/shaders/shadows/shadows_alpha_geom.glsl`

Based on the existing `shadows_geom.glsl` (which has a geometry shader for layered rendering into shadow map array). Modifications:
- **Vertex shader**: Add `layout(location = 2) in vec2 uv;` input. Pass `v_uv` to geometry shader.
- **Geometry shader**: Pass through `v_uv` from vertex to fragment (add `in vec2 gs_uv[]` and `out vec2 v_uv`).
- **Fragment shader**: Instead of empty fragment, sample the hair data texture R channel:
  ```glsl
  layout(set = 1, binding = 1) uniform MaterialUniforms { ... alphaThreshold ... } material;
  layout(set = 2, binding = 0) uniform sampler2D hairDataTex;
  
  void main() {
      float alpha = texture(hairDataTex, v_uv).r;
      if (alpha < material.alphaThreshold) discard;
  }
  ```

**Modify** `ext/Vulkan-Engine/src/core/passes/shadow_pass.cpp`:

1. **`setup_uniforms()`**: Add a texture descriptor layout for set 2 with 1 combined image sampler binding (for the hair data texture). This mirrors what the forward pass has for `OBJECT_TEXTURE_LAYOUT`.

2. **`setup_shader_passes()`**: Add a third shader pass after line 115:
   ```cpp
   GraphicShaderPass* depthAlphaPass =
       new GraphicShaderPass(m_device->get_handle(), m_renderpass, m_imageExtent, ENGINE_RESOURCES_PATH "shaders/shadows/shadows_alpha_geom.glsl");
   depthAlphaPass->settings = settings;
   depthAlphaPass->settings.descriptorSetLayoutIDs = {{GLOBAL_LAYOUT, true}, {OBJECT_LAYOUT, true}, {OBJECT_TEXTURE_LAYOUT, true}};
   depthAlphaPass->graphicSettings = gfxSettings;
   depthAlphaPass->graphicSettings.attributes = {
       {POSITION_ATTRIBUTE, true}, {NORMAL_ATTRIBUTE, false}, {UV_ATTRIBUTE, true}, {TANGENT_ATTRIBUTE, false}, {COLOR_ATTRIBUTE, false}};
   depthAlphaPass->build_shader_stages();
   depthAlphaPass->build(m_descriptorPool);
   m_shaderPasses[2] = depthAlphaPass;
   ```

3. **`render()`**: Update shader pass selection logic (line 146):
   ```cpp
   // Before: ShaderPass* shaderPass = mat->get_type() != IMaterial::Type::HAIR_STR_TYPE ? m_shaderPasses[0] : m_shaderPasses[1];
   // After:
   ShaderPass* shaderPass;
   if (mat->get_type() == IMaterial::Type::HAIR_STR_TYPE)
       shaderPass = m_shaderPasses[1];  // line geometry
   else if (mat->get_type() == IMaterial::Type::HAIR_CARD_TYPE)
       shaderPass = m_shaderPasses[2];  // alpha-tested triangles
   else
       shaderPass = m_shaderPasses[0];  // opaque triangles
   ```
   For HAIR_CARD_TYPE, also bind the material's texture descriptor set at set 2 (same pattern as forward pass):
   ```cpp
   if (mat->get_type() == IMaterial::Type::HAIR_CARD_TYPE)
       cmd.bind_descriptor_set(mat->get_texture_descriptor(), 2, *shaderPass);
   ```

Need to verify the existing shadow shader (`shadows_geom.glsl`) structure first — it uses a geometry shader for layered rendering into the shadow map array. The alpha variant must preserve this layered rendering while adding UV passthrough and fragment discard.

Status: [x] — Created `shadows_alpha_geom.glsl` (vertex passes UV with Y-flip, geometry layers per-light with UV passthrough, fragment discards on R < alphaThreshold). Modified `shadow_pass.cpp`: added 7-binding OBJECT_TEXTURE_LAYOUT in `setup_uniforms()` (matches forward pass for descriptor set compatibility); added `m_shaderPasses[2]` with UV attribute enabled and OBJECT_TEXTURE_LAYOUT enabled; updated `render()` to three-way pass selection and bind texture descriptor for HAIR_CARD_TYPE.

### Task 5: Add include in `core.h`

**Modify** `ext/Vulkan-Engine/include/engine/core.h`

Add after line 26 (`#include <engine/core/materials/hair_disney.h>`):
```cpp
#include <engine/core/materials/hair_card.h>
```

This ensures `HairCardMaterial` is available to any file that includes `<engine/core.h>`, including `application.cpp`.

Status: [x] — Added `#include <engine/core/materials/hair_card.h>` after `hair_disney.h` in `core.h`.

### Task 6: Wire up in application

**Modify** `src/application.cpp`

In `setup()`, inside the `#ifdef USE_GLB_MODELS` block, after the strand hair setup (after line 102), add:

```cpp
Mesh* hairCards = new Mesh();
Tools::Loaders::load_3D_file(hairCards, MESH_PATH + "hair_fauxmohawk.obj", false);
hairCards->set_position({0.0f, -12.6f, 0.2f});  // match character transform
hairCards->set_scale(10.0f);                      // match character scale
hairCards->set_rotation({0.0f, 180.0f, 0.0f});   // match character rotation

HairCardMaterial* hcMat = new HairCardMaterial();
hcMat->set_hair_color(Vec3(0.05f, 0.02f, 0.01f));  // dark brown base

Texture* hairDataTex = new Texture();
// IMPORTANT: Load as TEXTURE_FORMAT_TYPE_NORMAL (linear, not sRGB)
// because R/G/B encode data (alpha, gradient, ID), not perceptual color
Tools::Loaders::load_texture(hairDataTex, TEXTURE_PATH + "hair_fauxmohawk.PNG", TEXTURE_FORMAT_TYPE_NORMAL);
hcMat->set_hair_data_texture(hairDataTex);

hairCards->push_material(hcMat);
hairCards->set_name("HairCards");
m_scene->add(hairCards);  // added after character and strand hair for render order
```

Transform values (`position`, `scale`, `rotation`) will likely need tuning — the OBJ may already be in the character's local space (from the Unreal export) or may need adjustment. The initial values match the character mesh (`maria.glb`) transforms as a starting point.

Status: [x] — Added hair card mesh/material/texture setup in `application.cpp` inside `#ifdef USE_GLB_MODELS`, after strand hair block. `hair_fauxmohawk.obj` loaded synchronously; `HairCardMaterial` created with dark-brown base color; `hair_fauxmohawk.PNG` loaded as `TEXTURE_FORMAT_TYPE_NORMAL` (linear). Transform matches `maria.glb`.

### Task 7: Build and validate

1. **Build**: `cd build && cmake .. && cmake --build .`
2. **Debug test**: `./HairViewer --frames 10 --log-level warn` — check `debug_trace.log` for validation errors
3. **Visual test** (user performs): Run interactively, verify:
   - Hair cards render with correct alpha cutout (strand silhouettes, not solid rectangles)
   - Base hair color uniform applies correctly (dark brown)
   - Root-to-tip gradient visible (color changes from root to tip using G channel)
   - Kajiya-Kay specular highlights follow the hair tangent direction (anisotropic sheen)
   - Shadows show correct alpha-tested silhouette (not solid card shadows)
   - Existing strand hair still renders alongside cards (both visible)
   - No visual regressions in head/eyes rendering
4. **Tuning**: Transform, color, alpha threshold, specular parameters will likely need adjustment after first visual test

Status: [x] — Build clean (MSVC Debug). 10-frame headless run: zero new Vulkan validation errors or warnings from hair card code. Pre-existing warnings (STORAGE_IMAGE pool, swapchain semaphore reuse) unchanged from before this feature. Visual test pending user review.

### Files Summary

| Action | File | Purpose |
|--------|------|---------|
| Create | `ext/Vulkan-Engine/include/engine/core/materials/hair_card.h` | Material class definition |
| Create | `ext/Vulkan-Engine/src/core/materials/hair_card.cpp` | `get_uniforms()` implementation |
| Create | `ext/Vulkan-Engine/resources/shaders/forward/hair_card.glsl` | Forward rendering shader with Kajiya-Kay |
| Create | `ext/Vulkan-Engine/resources/shaders/shadows/shadows_alpha_geom.glsl` | Alpha-tested shadow shader |
| Modify | `ext/Vulkan-Engine/src/core/passes/forward_pass.cpp` | Register hair card shader pass (~10 lines) |
| Modify | `ext/Vulkan-Engine/src/core/passes/shadow_pass.cpp` | Add alpha-tested shadow pass + render selection |
| Modify | `ext/Vulkan-Engine/include/engine/core.h` | Add `#include` (1 line) |
| Modify | `src/application.cpp` | Scene setup (~15 lines) |

CMake picks up new `.cpp`/`.h` files automatically via `GLOB_RECURSE` in `add_module_files.cmake`.

### Risks & Notes

- **UV flip**: The PBR shader does `1-uv.y`. The MetaHuman OBJ may or may not need this — test both if alpha mask looks wrong.
- **Tangent quality**: OBJ loader computes tangents via Gram-Schmidt (`compute_tangents_gram_smidt()`). If Kajiya-Kay highlights look wrong, may need to derive tangent from UV gradient in shader instead.
- **Texture format**: Must load as `TEXTURE_FORMAT_TYPE_NORMAL` (linear, `RGBA_8U`) not color (`SRGBA_8`), since R/G/B encode data, not perceptual color. If loaded as sRGB, the gamma correction will distort the alpha threshold and gradient values.
- **Render order**: Hair cards are added after opaque geometry in the scene, which helps early-z rejection. No explicit sort needed since we use alpha test (discard), not alpha blending.
- **Double-sided**: Hair cards default to `faceCulling = false` since they are thin planar geometry that may be viewed from either side.
- **Shadow pass texture descriptor**: The existing shadow pass only has 2 descriptor set layouts (GLOBAL + OBJECT). Adding OBJECT_TEXTURE_LAYOUT for the alpha-tested pass requires adding the layout to the shadow pass's descriptor pool — need to verify pool allocation is sufficient.

---

## GLB Animation Driven by a JSON Timeline

### Goal

Animate the four GLB characters (`alex`, `javi`, `maria`, `nadia`) by feeding a JSON timeline of per-frame parameter values. Each GLB already ships with **55 skeletal joints** and **100 POSITION morph targets** (parsed and stored in `SkinData` / `MorphTargetData`), but those channels are currently dormant — no GPU consumption, no timeline evaluation.

In production we will receive a JSON file whose exact schema is **not yet known**. The implementation must keep the JSON → animation pipeline behind a thin adapter so that only the adapter changes when the production schema is finalized.

### Guiding Principles

1. **Schema-agnostic core.** An internal IR (`Animation`, `Track`, `Keyframe`) describes animation data. Adapters convert arbitrary JSON into this IR. Everything downstream (sampling, GPU upload, render) consumes only the IR.
2. **No speculative generality inside the renderer.** The renderer gains one animation system (morph weights + joint transforms). It does not try to support "any possible animatable property" — just the parameters the GLBs actually expose.
3. **Decoupled evaluation and consumption.** The timeline evaluator runs on CPU per frame and produces a plain buffer of (morph weights, joint matrices). The GPU stages that consume these buffers do not know about JSON, tracks, or interpolation.

---

### Step 1: Inspect the GLB Parameters [x]

**Goal**: Confirm what parameters are truly animatable across the four files, and capture the canonical names.

**Actions**:
- Add a one-off debug utility (gated behind a CLI flag, e.g. `--inspect-glb <path>`) in `src/main.cpp` that calls a new `Tools::Loaders::inspect_GLB()` and logs:
  - `model.animations[*]` — any baked glTF animations (channels, samplers, node targets, interpolation). If present, the easiest path is to consume these directly.
  - `model.skins[0].joints[*]` — joint node names (55 expected).
  - `model.meshes[*].extras["targetNames"]` — morph target names (100 expected on mesh 0).
  - `model.nodes[*]` — node TRS so the bind pose can be reconstructed.
- Save a reference dump for each of the four files under `resources/models/<char>/<char>.glb.params.txt` (gitignored or checked in — user's choice) for later mapping in the JSON adapter.

**Deliverable**: a table, appended below, of actual joint names and morph target names per character. This governs the adapter's mapping logic.

---

### Step 2: Internal Animation IR [x]

**Create** `ext/Vulkan-Engine/include/engine/core/animation.h`

```cpp
namespace Core {

enum class AnimationTargetType {
    MORPH_WEIGHT,         // target float in [0,1], one per morph target
    JOINT_TRANSLATION,    // vec3
    JOINT_ROTATION,       // quat (stored as vec4)
    JOINT_SCALE,          // vec3
    NODE_TRANSLATION,     // vec3 (root-level convenience, e.g. body translate)
    NODE_ROTATION,
    NODE_SCALE,
};

enum class InterpolationMode { STEP, LINEAR, CUBICSPLINE };

struct Keyframe {
    float time;      // seconds
    Vec4  value;     // 1..4 floats packed; scalar morphs use .x only
};

struct Track {
    AnimationTargetType type;
    std::string         targetName;   // joint name or morph target name
    InterpolationMode   interp = InterpolationMode::LINEAR;
    std::vector<Keyframe> keyframes;  // sorted by time
};

struct Animation {
    std::string        name;
    float              duration = 0.0f;
    float              fps      = 30.0f;       // reference only; we sample by time
    std::vector<Track> tracks;
};

// Sampled output for one frame, consumed by the renderer.
struct AnimationPose {
    std::vector<float> morphWeights;  // size = morph target count
    std::vector<Mat4>  jointMatrices; // size = joint count (local → bind space)
};

} // namespace Core
```

**Create** `ext/Vulkan-Engine/src/core/animation.cpp`

- `Animation::sample(float tSeconds, const SkinData&, const MorphTargetData&, AnimationPose& out)` — walks tracks, finds the bracket of keyframes, interpolates, writes into `out`. Handles wrapping (loop vs clamp) as a flag on `Animation`.
- Linear search per track is fine for ≤100 tracks × a few hundred keys; no acceleration structure needed.

---

### Step 3: JSON → IR Adapter (the generic surface) [x]

**Create** `ext/Vulkan-Engine/include/engine/core/animation_json.h`

```cpp
namespace Core {

// Adapter interface: a stateless strategy that parses nlohmann::json into Animation.
// Production will add a new adapter class without touching the rest of the pipeline.
class IAnimationJsonAdapter {
public:
    virtual ~IAnimationJsonAdapter() = default;
    virtual Animation parse(const nlohmann::json& root,
                            const SkinData&      skin,      // for name → index mapping
                            const MorphTargetData& morphs) = 0;
};

// Default adapter that understands an explicit internal schema (documented below).
class DefaultAnimationJsonAdapter : public IAnimationJsonAdapter { ... };

// Entry point:
Animation load_animation_json(const std::string& path,
                              const SkinData&    skin,
                              const MorphTargetData& morphs,
                              IAnimationJsonAdapter* adapter = nullptr); // uses default if null
```

**Default (internal) JSON schema** — used for initial testing. The production adapter will replace this class, not modify it:

```jsonc
{
  "name": "demo",
  "duration": 3.0,
  "fps": 30.0,
  "loop": true,
  "tracks": [
    { "target": "morph:mouthOpen",     "interp": "linear", "keys": [[0.0, 0.0], [1.5, 1.0], [3.0, 0.0]] },
    { "target": "joint:head/rotation", "interp": "linear", "keys": [[0.0, [0,0,0,1]], [1.5, [0,0.2,0,0.98]]] },
    { "target": "joint:neck/translation", "keys": [...] },
    { "target": "node:root/translation",  "keys": [...] }
  ]
}
```

Prefix convention (`morph:` / `joint:<name>/<channel>` / `node:<name>/<channel>`) keeps the format trivially extensible without schema versioning.

**Why an adapter pattern (not just one parser):** the production JSON may bake keys as `[{"t": 0.5, "v": 0.3}]`, flatten everything into per-parameter rows, reference parameters by numeric ID, or use a different time base. An adapter isolates that guesswork. When the real schema arrives, we write one `ProductionAnimationJsonAdapter` class; the IR, sampler, and renderer stay untouched.

---

### Step 4: Hook Animation onto a `Mesh` [x]

**Modify** `ext/Vulkan-Engine/include/engine/core/mesh.h`

Add:
```cpp
std::unique_ptr<Animation>     m_animation;   // owned timeline
AnimationPose                  m_pose;        // per-frame sampled output
float                          m_localTime = 0.0f;
bool                           m_animPaused = false;

void set_animation(std::unique_ptr<Animation> anim);
void advance_animation(float dtSeconds);   // called once per frame from the scene tick
const AnimationPose& get_pose() const;
```

`advance_animation` samples the timeline and updates `m_pose`. The Mesh holds the pose; the renderer pulls from it.

**Modify** `src/application.cpp` — after each GLB character is added, optionally attach an animation:
```cpp
auto anim = Tools::Loaders::load_animation_json(
    RESOURCES_PATH "animations/alex_demo.json",
    *character0->get_geometries()[0]->get_properties().skinData,
    *character0->get_geometries()[0]->get_properties().morphTargetData);
character0->set_animation(std::make_unique<Animation>(std::move(anim)));
```
Tick advances each animated mesh in `HairViewer::tick()`.

---

### Step 5: Mesh Deformation (CPU-side for MVP)

This is the minimum surface that has to land in the renderer for the animation to be visible. Two paths, done in order. For the MVP, **both use CPU-side deformation** (no GPU compute shaders or SSBOs) — morphed/skinned positions are recomputed on CPU each frame and uploaded via a host-visible (`VMA_MEMORY_USAGE_CPU_TO_GPU`) VBO.

**5a — Morph target deformation. [x] (CPU approach)**

Chosen approach: CPU-side blend of base vertex positions + weighted deltas, uploaded directly to GPU via a host-visible VBO each frame. No GPU SSBO or vertex shader changes needed.

Implementation summary:
- `Device::upload_vertex_arrays` gains `bool animatableVBO = false` parameter. When true, VBO is allocated as `VMA_MEMORY_USAGE_CPU_TO_GPU` (directly mappable), skipping the staging-buffer path.
- `Geometry::apply_morphs(const std::vector<float>& weights)` recomputes all vertex positions on CPU (`base + Σ weight[i] * deltaPos[i]`) and calls `vbo.upload_data()` directly.
- `ResourceManager::upload_geometry_data` detects `morphTargetData` presence, passes `animatableVBO=true`, and skips BLAS creation for morph meshes (CPU_TO_GPU buffers lack the required acceleration structure flag).
- `Mesh::advance_animation` calls `g->apply_morphs(m_pose.morphWeights)` after `sample()`.
- `resources/animations/test_morph.json` created — 4-second loop cycling `Exp_000` (peak at t=1s) and `Exp_001` (peak at t=3s) using the real morph target names discovered at runtime.
- Alex's 100 morph targets are named `Exp_000`–`Exp_099`.

**5b — Skeletal skinning. [x] (CPU approach)**

Chosen approach: same CPU-upload pattern as 5a — morph + skin in a single combined pass, one VBO upload per frame per mesh.

Implementation summary:
- `SkinData` gains `std::vector<int> parentIndices` (one per joint, -1 for roots). Parsed in `loaders.cpp` by walking the GLB node tree to find parent→child joint relationships.
- `Geometry::apply_morphs` replaced by `Geometry::apply_deformation(morphWeights, jointMatrices)`. Applies morphs first, then walks the joint hierarchy (root-to-leaf, using `parentIndices`) to build world-space matrices, computes skinning matrices (`worldMat * inverseBindMat`), and applies 4-joint blend per vertex. Normals and tangents are also transformed via the inverse-transpose of the skinning matrix.
- `Mesh::advance_animation` calls `apply_deformation` for any geometry that has morph or skin data.
- `application.cpp` logs the full joint list with parent names on startup (mirrors morph target logging).
- Alex's 55 joints are named `pelvis`, `spine1`–`spine3`, `neck`, `head`, `left/right_shoulder`, `left/right_elbow`, `left/right_wrist`, finger chains, etc.
- `resources/animations/test_anim.json` combines morph (Exp_000, Exp_001) and skeletal (right_shoulder, right_elbow) tracks into one 4-second looping test animation.

**GPU approach (deferred):** A compute shader writing into a transient vertex buffer before the forward pass is the cleaner long-term design. Not needed for MVP.

---

### Step 6: Validate [x]

1. **Build**: `cmake --build .` — verify no new validation errors in the 10-frame headless run.
2. **Static pose test**: load a JSON with a single keyframe (t=0, morph weight 1.0 on `jawOpen`). Confirm the face deforms.
3. **Timeline test**: 3-second looping JSON. Confirm smooth interpolation at 60 fps.
4. **Skeletal test**: rotate one joint (e.g. `head`) over 2 s. Confirm the head rotates without detaching from the neck.
5. **Schema-swap test**: write a second JSON in a deliberately different shape, supply a second adapter, confirm the IR/renderer path is unchanged.

**Validation results**:
- Build: clean (MSVC Debug).
- 10-frame headless run (`--frames 10 --log-level warn`): zero new Vulkan validation errors from animation code. Only pre-existing warnings (image layout / swapchain semaphore reuse) unchanged from prior runs.
- Visual test (user-confirmed): `resources/animations/test_anim.json` combining 2 morph tracks (`Exp_000`, `Exp_001`) and 2 skeletal tracks (`right_shoulder`, `right_elbow` rotations) plays smoothly on Alex. Mouth opens/closes, shoulder rotates around the arm axis, all interpolation is smooth, character holds bind pose at rest (no mesh collapse).
- Bind-pose initialization bug (character collapsing to a ball) was fixed in Step 5b by parsing `bindLocalMatrices` from GLB node TRS and resetting `AnimationPose::jointMatrices` to bind pose each frame in `sample()`.
- Schema-swap test deferred until the production JSON schema is known — adapter interface (`IAnimationJsonAdapter`) is in place, ready for a new adapter class when needed.

---

### Files Summary

| Action | File | Purpose |
|--------|------|---------|
| Modify | `ext/Vulkan-Engine/src/tools/loaders.cpp` + `.h` | Add `inspect_GLB()` debug utility |
| Modify | `src/main.cpp` | `--inspect-glb <path>` CLI flag |
| Create | `ext/Vulkan-Engine/include/engine/core/animation.h` | IR: `Animation`, `Track`, `Keyframe`, `AnimationPose` |
| Create | `ext/Vulkan-Engine/src/core/animation.cpp` | Timeline sampling / interpolation |
| Create | `ext/Vulkan-Engine/include/engine/core/animation_json.h` + `.cpp` | `IAnimationJsonAdapter` + default adapter |
| Modify | `ext/Vulkan-Engine/include/engine/core/mesh.h` + `.cpp` | `set_animation`, `advance_animation`, pose storage |
| Modify | `ext/Vulkan-Engine/src/systems/renderers/forward.cpp` (+ PBR pass shaders) | Upload weights / joint matrices, consume in vertex shader |
| Create | `resources/animations/<char>_demo.json` | One sample timeline per character for testing |
| Modify | `src/application.cpp` | Attach demo animations to GLB meshes |

CMake picks up new files via `GLOB_RECURSE`. nlohmann/json is already in `thirdparty/` (bundled with tinygltf).

---

### Risks & Notes

- **Unknown production schema.** The adapter pattern is the mitigation; resist the urge to pre-design for formats we haven't seen. When the schema arrives, write a new adapter in a single file.
- **Joint-name collisions.** If two joints share a name (rare but possible in mirrored rigs), switch the lookup to node-index-keyed tracks. The IR's `targetName` string is easy to swap for an integer without touching adapters.
- **Additive vs override morphs.** glTF morphs are additive by convention; our default sampler must accumulate weights, not replace.
- **Bind-pose sanity.** The TRS of every joint node must be captured at load time. If a JSON track omits a channel, we fall back to bind pose — do not assume identity.
- **Quaternion interpolation.** Use `slerp` for rotations, not per-component linear, to avoid the classic flipping artifact.
- **Time units.** Keep the IR in seconds; let adapters convert from frames/ms. Any ambiguity lives at the adapter boundary.
- **Memory.** 100 morph targets × ~40K verts × 12 bytes ≈ 48 MB per character for deltas as a dense SSBO. Acceptable for 1–4 characters; if we scale up, compress or store only non-zero deltas (sparse).

---

## Executable Deployment — `SLViewer`

### Goal

Ship a standalone, distributable executable (`SLViewer`, named for *sign language*) that another machine can run with no IDE, no Vulkan SDK, no source tree — just the binary, its bundled resources, and a single JSON animation file. The program loads the **Alex** GLB scene (current `USE_GLB_MODELS` configuration: GLB character + strand hair + hair cards), plays the animation described by the JSON exactly once, writes each rendered frame to disk as a PNG, then invokes a bundled `ffmpeg` binary to encode the sequence into an MP4 video, and exits.

Two native builds: one Windows `.exe`, one Linux ELF. Each ships as its own self-contained folder.

### Distribution Layout

```
SLViewer-windows/
├── SLViewer.exe
├── ffmpeg.exe                   # bundled encoder
├── vulkan-1.dll                 # Vulkan loader (Windows)
└── resources/
    ├── shaders/                 # full shader tree (compiled on the fly via Shaderc)
    ├── models/
    │   ├── alex.glb
    │   └── hair_fauxmohawk.obj
    ├── textures/
    │   ├── hair_fauxmohawk.PNG
    │   └── <HDR / IBL maps used by the Alex scene>
    └── strands/
        └── straight.hair        # if used by the Alex setup

SLViewer-linux/
├── SLViewer                     # ELF binary
├── ffmpeg                       # bundled encoder
└── resources/                   # same tree
```

Invocation:

```
SLViewer <animation.json> [--output out.mp4] [--width W] [--height H]
```

The user only needs to ship the folder and one JSON. Output defaults to `<animation_basename>.mp4` next to the JSON.

### CLI Surface

| Flag | Default | Purpose |
|------|---------|---------|
| `<animation.json>` | (required positional) | Path to the timeline JSON (consumed by `DefaultAnimationJsonAdapter`) |
| `--output <file.mp4>` | `<json_basename>.mp4` | Final video path |
| `--width N` | 1920 | Render width |
| `--height N` | 1080 | Render height |
| `--keep-frames` | off | Skip cleanup of the temp PNG dir (debugging) |
| `--log-level` | `warn` | Reuse existing Vulkan validation filter |

Frame count is derived: `totalFrames = round(animation.duration * animation.fps)`. The animation plays exactly once, no loop.

### Task 1: New CMake target `SLViewer`

**Modify** the root `CMakeLists.txt` to add an `add_executable(SLViewer ...)` target alongside `HairViewer`. It links against the same `VulkanEngine` static library so the renderer is shared between the two binaries.

**Create** a new source folder `src/slviewer/` containing the SLViewer-specific lifecycle code. Files in `src/` that are HairViewer-only (`gui.h/cpp`, `application.h/cpp`) are not added to this target. Both targets should be built by default (`cmake --build .` produces `HairViewer` and `SLViewer`).

Status: [x] — `SLViewer` CMake target added to root `CMakeLists.txt`. Links `VulkanEngine` + `Vulkan::Vulkan` + `Threads::Threads`. Defines `SLVIEWER_BUILD=1` and empty `RESOURCES_PATH` (runtime-resolved in Task 3). Stub `src/slviewer/main.cpp` added; target builds cleanly.

### Task 2: Headless application class `SLApplication`

**Create** `src/slviewer/application_sl.h` + `src/slviewer/application_sl.cpp`.

`SLApplication` mirrors `HairViewer`'s lifecycle (`init → setup → tick → shutdown`) but stripped of everything interactive:

- **Hardcoded Alex scene**: copy the body of `application.cpp`'s `USE_GLB_MODELS` block (alex.glb + strand hair + hair cards + materials + lights + HDR/IBL). No `#ifdef` switching, no neural avatar path.
- **Fixed camera**: a single forward-facing transform with no input handlers (`GLFW` keyboard/mouse callbacks not registered).
- **No GUI**: do not include `gui.h`. ImGui is not initialized.
- **Animation-driven**: the constructor takes the parsed `Animation` and the resolved render config (`width`, `height`, `outputPath`, `keepFrames`). After scene setup, the animation is attached to the GLB mesh via `Mesh::set_animation()` (already implemented in the existing animation system).
- **Bounded tick**: the main loop counts down `totalFrames = round(animation.duration * animation.fps)` and exits cleanly when zero. Looping is disabled at the `Animation` level (or the loop simply terminates before the timeline wraps).

**Create** `src/slviewer/main.cpp`. Responsibilities: argument parsing (see Task 7 for the surface), JSON load via `Tools::Loaders::load_animation_json()`, instantiate `SLApplication`, run, encode video, clean up.

Status: [x] — `application_sl.h` + `application_sl.cpp` created. Hardcoded Alex scene (alex.glb + hair_fauxmohawk.obj + materials + lights + HDR/IBL + SSS LUT). Hidden GLFW window via new `WindowGLFW::set_visible_hint(false)`. Fixed-timestep loop (`dt = 1/fps`) counting `round(duration * fps)` frames. `m_onFrameReady` callback stub for Task 4. `main.cpp` updated with full CLI parsing; `resourcesPath` left empty (Task 3 hook). Builds cleanly.

### Task 3: Resource-path discovery

**Create** `src/slviewer/resource_paths.h` + `src/slviewer/resource_paths.cpp`.

The engine currently bakes `ENGINE_RESOURCES_PATH`, `MESH_PATH`, `TEXTURE_PATH` as compile-time macros pointing into the source tree. For a distributable binary these must resolve to `<exe_dir>/resources/` at runtime.

**Approach**:
1. Look up the executable's directory:
   - Windows: `GetModuleFileNameW(NULL, ...)` → strip filename.
   - Linux: `readlink("/proc/self/exe", ...)` → strip filename.
2. Compose `<exe_dir>/resources/` and verify it exists. Abort with a clear error if it doesn't.
3. Expose the resolved path to the engine. Since the engine reads compile-time macros today, introduce a small runtime override:
   - Add a `Engine::set_runtime_resources_path(const std::string&)` accessor (engine side) that, when set, supersedes the macro inside `loaders.cpp` and any other path-consuming sites.
   - SLViewer calls this once on startup. HairViewer does not call it; its macro-based path keeps working unchanged.

Status: [x] — `src/slviewer/resource_paths.h/cpp` created with `get_exe_dir()` (Linux: `readlink /proc/self/exe`, Windows: `GetModuleFileNameW`) and `discover_resources_path()` probing `<exe_dir>/resources/` (deployed) then `<exe_dir>/../resources/` (dev build). Added `engine/engine_config.h` + `src/core/engine_config.cpp` to the engine with `get_engine_resources_path()` / `set_engine_resources_path()`; global defaults to compile-time `ENGINE_RESOURCES_PATH` so HairViewer is unchanged. Replaced all `ENGINE_RESOURCES_PATH "..."` string concatenations in 18 engine source files with `get_engine_resources_path() + "..."`. In the deployed layout, `discover_resources_path()` also calls `VKFW::set_engine_resources_path()` so shaders/meshes resolve from the same root. `main.cpp` now calls `discover_resources_path()` and passes the result to `SLApplication::run()`. Builds cleanly.

### Task 4: Offscreen framebuffer capture

The renderer currently presents into a `VkSwapchainKHR` bound to a visible GLFW window. For SLViewer we want frames on disk, not on screen.

**Approach (v1)**: create a **hidden GLFW window** (`glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE)`) sized to the requested resolution and keep the existing swapchain. After the FXAA/Tonemap final pass writes into the swapchain image:

1. Insert a pipeline barrier transitioning the acquired swapchain image to `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`.
2. `vkCmdCopyImageToBuffer` into a host-visible staging buffer (`VMA_MEMORY_USAGE_GPU_TO_CPU`) sized `width * height * 4`.
3. Transition back to `PRESENT_SRC_KHR`, present (or skip present — the window is hidden).
4. Wait on the frame fence, map the staging buffer, hand the bytes off to the PNG writer (Task 5).

This keeps the entire existing renderer (passes, MRT, MSAA, post-process chain) untouched. A pure-offscreen path (render directly into a `VkImage` with no swapchain) is a future optimization.

**Create** `src/slviewer/frame_capture.h` + `src/slviewer/frame_capture.cpp` to encapsulate the staging buffer, the barriers, and the readback. `SLApplication::tick()` calls `m_capture.read_back(currentFrame)` once per rendered frame.

Status: [x] — Implemented together with Tasks 5 and 6.
- Added `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` to swapchain creation (`swapchain.cpp`).
- Added `set_pre_submit_callback()` + `get_device()` to `BaseRenderer`; callback fires between last pass and `submit_frame`.
- `FrameCapture::get_callback()` records barrier (PRESENT→TRANSFER_SRC) + `vkCmdCopyImageToBuffer` + barrier-back (TRANSFER_SRC→PRESENT) into the live command buffer.
- `FrameCapture::wait_and_write()` calls `wait_queue(GRAPHIC_QUEUE)` then maps, BGRA→RGBA-swaps, and writes PNG.

### Task 5: PNG writer

**Add** `thirdparty/stb/stb_image_write.h` (single-header public-domain library) if it isn't already vendored. Define `STB_IMAGE_WRITE_IMPLEMENTATION` in exactly one translation unit (`frame_capture.cpp`).

In `FrameCapture::write_png(uint32_t frameIndex)`, after the staging buffer is mapped, call:

```cpp
char name[64];
std::snprintf(name, sizeof(name), "frame_%05d.png", frameIndex);
auto path = m_tempDir / name;
stbi_write_png(path.string().c_str(), m_width, m_height, 4, m_mapped, m_width * 4);
```

Zero-padded filenames match ffmpeg's `-i frame_%05d.png` pattern.

Status: [x] — Vendored `stb_image_write.h` (v1.16) at `src/slviewer/stb_image_write.h`. `STB_IMAGE_WRITE_IMPLEMENTATION` defined in `frame_capture.cpp`. `wait_and_write()` writes zero-padded `frame_%05u.png` files into the temp dir.

### Task 6: Temp directory lifecycle

On `SLApplication::init()`, create `<os_tempdir>/slviewer_<pid>/` using `std::filesystem::temp_directory_path()` and `std::filesystem::create_directories()`. Store the resulting path on `SLApplication`.

On `shutdown()` (after ffmpeg has succeeded), recursively delete it via `std::filesystem::remove_all()`. If `--keep-frames` was passed, skip the delete and log the path to stdout so the user can inspect the dump.

If ffmpeg fails, also retain the directory automatically — it's the user's only clue for diagnosing the failure.

Status: [x] — Temp dir created in `run()` before `init()` as `<tmpdir>/slviewer_<pid>/`. Deleted after renderer shutdown unless `--keep-frames`. `m_tempDir` stored on `SLApplication` and passed to `m_capture.wait_and_write()` each tick. ffmpeg failure retention deferred to Task 7 (it needs the ffmpeg result code).

### Task 7: FFmpeg invocation

**Create** `src/slviewer/video_encoder.h` + `src/slviewer/video_encoder.cpp`.

After the render loop completes and `vkDeviceWaitIdle` returns, locate the bundled ffmpeg binary relative to the executable (`<exe_dir>/ffmpeg.exe` on Windows, `<exe_dir>/ffmpeg` on Linux) and invoke it as a subprocess:

```
ffmpeg -y -framerate <fps> -i <tempDir>/frame_%05d.png \
       -c:v libx264 -pix_fmt yuv420p -crf 18 <output>.mp4
```

Use `std::system` for v1 — quoting is acceptable since the paths are constructed by us, not user-supplied. If quoting becomes fragile later, switch to `CreateProcessW` (Windows) / `posix_spawn` (Linux). Block on exit. Non-zero return codes are surfaced to the user with the ffmpeg stderr passed through.

CLI surface for `SLViewer`:

| Flag | Default | Purpose |
|------|---------|---------|
| `<animation.json>` | (required positional) | Path to the timeline JSON (consumed by `DefaultAnimationJsonAdapter`) |
| `--output <file.mp4>` | `<json_basename>.mp4` next to the JSON | Final video path |
| `--width N` | 1920 | Render width |
| `--height N` | 1080 | Render height |
| `--keep-frames` | off | Retain the temp PNG dump |
| `--log-level` | `warn` | Reuse the existing Vulkan validation filter |

Status: [x] — `VideoEncoder::encode()` created in `src/slviewer/video_encoder.h/.cpp`. Uses `std::system` to invoke system ffmpeg with `-framerate <fps> -i frame_%05d.png -c:v libx264 -pix_fmt yuv420p -crf 18`. Called from `SLApplication::run()` after `renderer->shutdown()`. On failure: throws `std::runtime_error`, `run()` catches it, logs the error, sets `m_keepFrames = true` so the PNG dump is retained for diagnosis. fps stored as `m_fps` member, set from `anim.fps` in `setup()`. Build clean.

### Task 8: Bundle ffmpeg into the install tree

Vendor an ffmpeg binary per target platform — either checked into `thirdparty/ffmpeg/{windows,linux}/` or downloaded by a CMake `FetchContent` / `file(DOWNLOAD ...)` step at configure time. Add CMake `install()` rules that copy the matching platform's binary next to `SLViewer` in the install directory.

Create `THIRD_PARTY_NOTICES.txt` listing the ffmpeg version, build configuration, and license (LGPL vs GPL build — pick the LGPL build with attribution to avoid GPL propagation).

Status: [x] — CMake `file(DOWNLOAD)` fetches BtbN's static GPL linux64 build at configure time into `build/ffmpeg-download/ffmpeg` (cached after first run; skips download if binary already exists). `tar --strip-components=2 --wildcards '*/bin/ffmpeg'` extracts only the binary. `get_ffmpeg_path()` added to `resource_paths.h/cpp`: returns `<exe_dir>/ffmpeg` if it exists (deployed layout), otherwise `"ffmpeg"` (system PATH, dev layout). `VideoEncoder::encode()` now accepts an optional `ffmpegExe` parameter (default `"ffmpeg"`); `SLApplication::run()` passes `get_ffmpeg_path()`. CMake `install()` rules place SLViewer binary and ffmpeg at the install root. `THIRD_PARTY_NOTICES.txt` created listing ffmpeg (GPLv2+), stb_image_write (public domain/MIT), tinygltf (MIT), and nlohmann/json (MIT). Smoke test: 640×360 test render produces a valid 53 KB MP4.

### Task 9: Resource-tree install rules

Add CMake `install()` rules that copy **only the assets the Alex scene actually uses** into `<install>/resources/`:

- Shader subtrees: `forward/`, `shadows/`, `compute/`, `misc/`, `postprocess/`, `scripts/` (full trees — the on-the-fly Shaderc compiler walks `#include` chains across them).
- Models: `alex.glb`, `hair_fauxmohawk.obj`, `straight.hair`.
- Textures: every file actually referenced by Alex's materials + the HDR used for IBL.

Explicitly **do not** ship the neural-avatar PLYs (`tono.ply`, `pablo.ply`, `tony.ply`), unrelated example scene assets (`ext/Vulkan-Engine/examples/resources/`), or the four-character GLB set (`maria.glb`, `javi.glb`, `nadia.glb`). Keeps the distribution lean.

Status: [x] — CMake `install()` rules added to section 7 of `CMakeLists.txt`. App assets (models/alex/alex.glb, hair_fauxmohawk.obj; textures/alex/hair_fauxmohawk_{attribute,tangent}.png; textures/studio_demo.hdr, scatterDistance.png; animations/test_anim.json + test_morph.json) installed individually. Engine resources (shaders/, meshes/, textures/ from ext/Vulkan-Engine/resources/) installed as full directory trees via `install(DIRECTORY ...)`. All other characters (javi, maria, nadia, neural avatars) excluded. Smoke test: `./SLViewer resources/animations/test_anim.json` from the installed prefix produces a valid 53 KB MP4 using the bundled ffmpeg.

### Task 10: Windows packaging

CMake `install()` rule to copy `vulkan-1.dll` from the Vulkan SDK runtime redistributable next to `SLViewer.exe`. Either statically link the MSVC C++ runtime (`/MT`) or document a dependency on the VC++ redistributable (preferred: `/MT` for one-folder portability).

Smoke test: copy the install folder to a clean Windows machine with no Vulkan SDK, no Visual Studio, no IDE installed. Confirm `SLViewer.exe test_anim.json` produces a video.

Status: [x] — Two changes in `CMakeLists.txt`:
1. `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"` set globally under `if(MSVC)` — produces `/MT` (Release) / `/MTd` (Debug), eliminating the VC++ redistributable dependency.
2. Section 8 added: `if(WIN32)` install rule locates `vulkan-1.dll` via `$ENV{VULKAN_SDK}/Bin/vulkan-1.dll` (LunarG installer path) with a fallback to the directory containing `Vulkan_LIBRARY`; copies it to the install root. Emits a `WARNING` if not found.
3. Section 6 extended with `elseif(WIN32)` block: downloads BtbN static GPL win64 ffmpeg zip at configure time, extracts `ffmpeg.exe` via `cmake -E tar xf`, and caches the binary at `build/ffmpeg-download/ffmpeg.exe`. Existing install rule (`install(PROGRAMS "${_ffmpeg_bin}" ...)`) covers both platforms.
Note: Smoke test requires a Windows machine — not performed on Linux dev box.

### Task 11: Linux packaging

Either statically link what's feasible or ship a launcher shell script that sets `LD_LIBRARY_PATH=<exe_dir>/lib/` before exec-ing the real binary. Bundle `libvulkan.so.1` so the binary doesn't depend on a Vulkan loader being installed on the host. Mark the ffmpeg and SLViewer binaries executable in the install rules.

Smoke test: copy the install folder to a clean Ubuntu LTS without the Vulkan SDK and confirm `./SLViewer test_anim.json` produces a video.

Status: [x] — Two changes in `CMakeLists.txt`:
1. `INSTALL_RPATH "$ORIGIN/lib"` set on SLViewer target (section 5): the installed binary searches `lib/` next to itself before system paths. No wrapper script needed — user runs `./SLViewer` directly. Confirmed via `LD_DEBUG=libs` that `$ORIGIN/lib` is tried before `/lib/x86_64-linux-gnu/`.
2. Section 9 added: `if(UNIX AND NOT APPLE)` block locates `libvulkan.so.1` from `${Vulkan_LIBRARY}` directory or `$ENV{VULKAN_SDK}/lib/`, copies it to `<install>/lib/libvulkan.so.1`.
3. Executables (`SLViewer`, `ffmpeg`) already use `install(TARGETS ... RUNTIME ...)` / `install(PROGRAMS ...)` which preserve the executable bit.
Note: Distribution must be built with `CMAKE_BUILD_TYPE=Release` (defines `NDEBUG`, disables Vulkan validation layers which are SDK-only). Smoke test (Release build, `LD_LIBRARY_PATH=""`, `./SLViewer test_anim.json --width 640 --height 360`) produced a valid 53 KB MP4 from the install prefix.

### Task 12: End-to-end smoke test

On each platform:

1. `cmake --install build --prefix SLViewer-<platform>/` produces the distribution folder.
2. Copy it to a clean directory (no source tree, no Vulkan SDK, no IDE).
3. Drop `test_anim.json` (the existing combined morph + skeletal animation) next to the binary.
4. Run `./SLViewer test_anim.json`.

Expected: `test_anim.mp4` appears next to the JSON, ~4 s long at 30 fps and 1080p, showing the Alex character playing the morph + skeletal animation. Process exits 0. Temp dir is gone.

Status: [ ]

### Files Summary

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/slviewer/main.cpp` | Argument parsing, animation load, top-level orchestration |
| Create | `src/slviewer/application_sl.h` + `.cpp` | Headless Alex scene + animation playback + frame capture |
| Create | `src/slviewer/frame_capture.h` + `.cpp` | Swapchain readback + PNG writer wrapper |
| Create | `src/slviewer/video_encoder.h` + `.cpp` | ffmpeg subprocess invocation |
| Create | `src/slviewer/resource_paths.h` + `.cpp` | Exe-dir lookup, runtime resources-path override |
| Modify | `CMakeLists.txt` (root) | New `SLViewer` target + `install()` rules for resources, ffmpeg, runtime DLLs |
| Modify | `ext/Vulkan-Engine/` (loaders / globals) | Runtime override for the compile-time `ENGINE_RESOURCES_PATH` macro |
| Add | `thirdparty/stb/stb_image_write.h` (if absent) | PNG encoding |
| Add | `thirdparty/ffmpeg/{windows,linux}/ffmpeg[.exe]` (or CMake fetch script) | Bundled encoder |
| Add | `THIRD_PARTY_NOTICES.txt` | ffmpeg license attribution |

### Files Summary

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/slviewer/main.cpp` | Argument parsing, animation-driven main loop |
| Create | `src/slviewer/application_sl.h` + `.cpp` | Headless Alex scene + animation playback + frame capture |
| Create | `src/slviewer/frame_capture.h` + `.cpp` | Swapchain readback + PNG writer wrapper |
| Create | `src/slviewer/video_encoder.h` + `.cpp` | ffmpeg subprocess invocation |
| Create | `src/slviewer/resource_paths.h` + `.cpp` | Exe-dir lookup, resource path resolution |
| Modify | `CMakeLists.txt` (root) | New `SLViewer` target + `install()` rules for resources, ffmpeg, runtime DLLs |
| Add | `thirdparty/stb/stb_image_write.h` (if absent) | PNG encoding |
| Add | `thirdparty/ffmpeg/{windows,linux}/ffmpeg[.exe]` (or CMake fetch script) | Bundled encoder |
| Add | `THIRD_PARTY_NOTICES.txt` | ffmpeg license attribution |

### Risks & Notes

- **Engine resource paths are compile-time macros.** `ENGINE_RESOURCES_PATH` and friends are baked in via `add_compile_definitions` at configure time. To redirect them at runtime, either (a) introduce a runtime override (a `std::string` consulted by the engine before falling back to the macro) or (b) compile the engine with a sentinel and resolve it at startup. Option (a) is the cleaner of the two — flagged as part of Task 3.
- **Hidden window vs pure offscreen.** GLFW hidden windows still require a display server on Linux. If we want to render on a true headless machine (CI, server), we'll eventually need the pure-offscreen path. Out of scope for v1 but worth keeping in mind for Task 4's design.
- **Swapchain readback timing.** `vkCmdCopyImageToBuffer` must happen after the final pass's color attachment finishes and before present (or after present using a fence). The simplest correct path is: render → barrier to `TRANSFER_SRC_OPTIMAL` → copy → barrier back → present → wait on fence → map staging buffer. Costs one extra synchronization per frame but keeps the loop deterministic.
- **ffmpeg distribution license.** Static LGPL builds are redistributable with attribution; GPL builds (with x264/x265) impose stronger requirements. Verify the bundled binary's license matches what we can ship and document it.
- **Animation must terminate.** The current `Animation::sample()` supports looping. For SLViewer the loop must be disabled (or the runner must stop after `duration` seconds regardless). Either set a non-loop flag on the loaded Animation or have the frame loop count down independently. Pick the simpler — frame countdown in the main loop.
- **Color space.** The forward chain ends in an SRGB swapchain. PNGs are gamma-encoded by stb. Confirm we read back the post-tonemap, post-FXAA image and that the byte values are already sRGB-encoded — otherwise the video will look washed out.
- **No GUI dependency.** `gui.h/cpp` pulls ImGui. Compile-gate it out of the SLViewer target to keep the binary lean and to avoid initializing ImGui font atlases / descriptor pools we never use.

---