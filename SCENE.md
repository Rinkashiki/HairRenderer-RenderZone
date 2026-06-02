# Scene JSON Module

The renderer loads scenes from a JSON file at runtime. `HairViewer` loads
`resources/scenes/default.json` unconditionally; `SLViewer` accepts an
optional `--scene <path>` flag and falls back to the same default. The parser
lives in the application layer (`src/scene_loader.{h,cpp}`).

---

## 1. Files

| Path | Purpose |
|------|---------|
| `src/scene_loader.h` / `.cpp` | Parser. Returns `{Scene*, Camera*, Mesh* primaryAnimated, Vec4 clearColor}`. |
| `resources/scenes/default.json` | Bundled default scene (Alex + hair cards + 1 point light + skybox + test animation). |
| `resources/scenes/*.json` | Alternative scenes (`javi`, `maria`, `nadia`, `neural_tono`, `bust_strands`). |
| `src/application.cpp` | HairViewer wiring (always loads `default.json`). |
| `src/slviewer/application_sl.cpp` | SLViewer wiring (`--scene` flag overrides default). |

Loading entry point:

```cpp
scene_loader::LoadResult result = scene_loader::load_scene_json(
    scenePath,
    resourcesPath,           // trailing slash; root for relative mesh/texture refs
    engineResourcesPath,     // trailing slash; root for engine built-ins (sphere.obj)
    animationOverride,       // empty for HairViewer; SLViewer's positional arg otherwise
    renderer);               // optional; receives `renderer.sss_scatter_lut`
```

The parser **tolerates unknown keys** (logs a `WARN` and ignores them) and
**hard-fails on missing required fields** (`name`, mesh `file`, mesh `type`,
material `type`, light `type`).

---

## 2. Top-level schema

```json
{
  "name":     "alex_default",
  "camera":   { ... },
  "lights":   [ ... ],
  "meshes":   [ ... ],
  "scene":    { ... },
  "renderer": { ... }
}
```

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `name` | no | `"unnamed"` | Display label. |
| `camera` | no | engine default | See §3. |
| `lights` | no | `[]` | Array. See §4. |
| `meshes` | no | `[]` | Array. See §5. |
| `scene` | no | engine defaults | Ambient, skybox, fog, IBL. See §7. |
| `renderer` | no | — | Clear color, SSS LUT. See §8. |

---

## 3. Camera

```json
"camera": {
  "position": [0.0, 0.0, -16.0],
  "near":     0.1,
  "far":      100.0,
  "fov":      40.0
}
```

All fields optional. `fov` is vertical field-of-view in degrees.

---

## 4. Lights

```json
"lights": [
  { "type": "point", ... },
  { "type": "directional", ... }
]
```

### 4.1 Common fields

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `type` | string | (required) | `"point"` or `"directional"`. |
| `name` | string | engine default | |
| `position` | vec3 | `[0,0,0]` | |
| `color` | vec3 | `[1,1,1]` | |
| `intensity` | float | engine default | |
| `cast_shadows` | bool | `true` | |
| `shadow_fov` | float | engine default | Degrees. |
| `shadow_bias` | float | engine default | |
| `shadow_near` | float | engine default | |
| `shadow_far` | float | engine default | |
| `dummy_mesh` | string | (none) | Optional marker mesh from the **engine** resources (e.g. `"sphere.obj"`). Loaded as a child of the light with an `UnlitMaterial` and `cast_shadows(false)` — matches the existing light-marker pattern. |
| `dummy_visible` | bool | `true` | Initial active state of the `dummy_mesh` orb. Set to `false` to hide it on load; the GUI checkbox still works to toggle it back on. Ignored when `dummy_mesh` is absent. |

### 4.2 Type-specific

| Type | Extra fields |
|------|--------------|
| `point` | `area_of_effect` (float) |
| `directional` | `direction` (vec3, default `[0,-1,0]`) |

---

## 5. Meshes

```json
"meshes": [
  {
    "name":     "Alex",
    "type":     "glb",
    "file":     "models/alex/alex.glb",
    "glb_mesh_index": 0,
    "position": [0.0, -12.6, 0.2],
    "scale":    10.0,
    "rotation": [0.0, 180.0, 0.0],
    "material": { ... },
    "animation": "animations/test_anim.json",
    "children": [ ... ]
  }
]
```

### 5.1 Loader dispatch

| `type` | Loader | Notes |
|--------|--------|-------|
| `glb` | `Tools::Loaders::load_GLB` | `glb_mesh_index` (int, default `-1` = all). Embedded textures populated for `$GLB[N]` references. |
| `obj` / `ply` / `hair` | `Tools::Loaders::load_3D_file` (sync) | Dispatched by extension. |
| `neural_hair` | `hair_loaders::load_neural_hair` (detached thread) | Project-specific PLY format. Optional flags: `preload`, `verbose`, `calculate_tangents`, `save_output`. |

`file` is **relative to `resourcesPath`**.

### 5.2 Common fields

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `name` | string | engine default | |
| `position` | vec3 | `[0,0,0]` | World-space (or parent-local if child). |
| `scale` | float \| vec3 | `[1,1,1]` | Scalar = uniform. |
| `rotation` | vec3 | `[0,0,0]` | Euler **degrees**, applied X→Y→Z. |
| `material` | object | (none) | See §6. |
| `animation` | string | (none) | **Optional**. Path to animation JSON (relative to `resourcesPath`). See §9. |
| `active` | bool | `true` | |
| `cast_shadows` | bool | `true` | |
| `affected_by_fog` | bool | `true` | |
| `children` | array | `[]` | Nested meshes. Transforms inherited from this parent. |
| `attach_to` | object | (none) | Parent this mesh to a specific joint of another mesh's skeleton — useful for hair/glasses/hats that should follow an animated bone. Shape: `{ "mesh": "<top-level mesh name>", "joint": "<joint name>" }`. The mesh's `position` / `rotation` / `scale` then act as **local offset relative to that joint**, so existing values may need re-tuning after attaching. Resolved after every mesh is built, so forward references are fine. Requires the referenced mesh to carry skinning data (i.e. a GLB with a skeleton). |

---

## 6. Materials

Discriminated by `"type"`:

| `type` | Class |
|--------|-------|
| `pbr` | `PhysicallyBasedMaterial` |
| `haircard` | `HairCardMaterial` |
| `hairepic` | `HairEpicMaterial` |
| `hair` | `HairMaterial` (Marschner) |
| `hairdisney` | `HairDisneyMaterial` |
| `unlit` | `UnlitMaterial` |

### 6.1 `pbr`

| Field | Type | Default |
|-------|------|---------|
| `albedo` | vec3 | `[1,1,0.5]` |
| `albedo_texture` | texref | (none) |
| `albedo_weight` | float | `1.0` |
| `opacity` | float | `1.0` |
| `opacity_weight` | float | `0.0` |
| `metalness` | float | `0.5` |
| `metallic_texture` | texref | (none) |
| `metalness_weight` | float | `1.0` |
| `roughness` | float | `0.5` |
| `roughness_texture` | texref | (none) |
| `roughness_weight` | float | `1.0` |
| `occlusion` | float | `1.0` |
| `occlusion_texture` | texref | (none) |
| `occlusion_weight` | float | `1.0` |
| `normal_texture` | texref | (none) |
| `emissive_color` | vec3 | `[0,0,0]` |
| `emissive_texture` | texref | (none) |
| `emissive_weight` | float | `1.0` |
| `emission_intensity` | float | `1.0` |
| `reflective` | bool | `false` |

**Skin realism — authored maps (Layer A).** All optional; absence falls back to the standard non-skin PBR path. Loaded as linear data maps (not sRGB).

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `bent_normal_texture` | texref | (none) | Tangent-space bent-normal map. When present, IBL diffuse ambient samples the irradiance cube along the bent normal so crevices read incoming light from the open-sky direction; Fresnel/specular keeps the geometric normal. |
| `curvature_texture` | texref | (none) | Grayscale `0=flat, 1=curved`. Drives the analytical pre-integrated diffuse (Penner GDC 2011) — replaces Lambert NdotL with a curvature-aware per-channel wrapped response. Produces the warm red wraparound at shadow terminators on curved features (nose, cheeks, jaw). |
| `scattering_texture` | texref | (none) | Per-texel SSS modulation. White = full subsurface scatter, black = none. Sampled in the forward pass into `outAlbedoMask.a`; the SSS post-process lerps between local diffuse and scattered diffuse by this value. |
| `clothes_mask_texture` | texref | (none) | Region mask, white = clothes, black = skin. Gates **all** skin-specific effects (pre-integrated diffuse, bent-normal IBL, screen-space SSS) on clothing regions painted into the same mesh material. Smooth lerp across the boundary so no hard seam appears. |

**Skin realism — microdetail (Layer B).** Pore-scale realism on top of the base normal. Detail normal + cavity share the same `detail_tiling` so the cavity dips align with the normal-map indentations.

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `detail_normal_texture` | texref | (none) | Tileable high-frequency tangent-space normal blended via "whiteout" blend over the base normal in tangent space. |
| `detail_cavity_texture` | texref | (none) | Tileable grayscale cavity (white = flat, dark = pore). Feeds both per-light specular occlusion and per-pixel SSS sample weight. |
| `detail_tiling` | float | `8.0` | UV multiplier for both detail textures. Higher = smaller pores. `16`–`32` is typical for face close-ups. |
| `detail_normal_strength` | float | `0.5` | Strength of the detail-normal blend, `[0, 1]`. `0` = base normal only, `1` = full detail. |
| `cavity_spec_occlusion` | float | `1.0` | How strongly cavity attenuates the per-light specular term. `0` = no attenuation, `1` = specular zeroed in fully-dark cavity regions. |
| `cavity_sss_attenuation` | float | `0.0` | How strongly cavity attenuates the per-pixel SSS sample weight written into `outDiffuseIrr.a`. `0` = SSS ignores cavity, `1` = SSS fully scaled by cavity (no scatter in pore crevices). |
| `dual_lobe_mix` | float | `0.0` | Penner GDC 2011 dual-lobe specular weight. `0` = single (default) GGX lobe, `>0` mixes a softer second lobe at `dual_lobe_roughness_soft`. `0.15` is a subtle skin sheen, `0.3` is strong. Gated by clothes mask. |
| `dual_lobe_roughness_soft` | float | `0.55` | Roughness of the soft secondary GGX lobe. |

All Layer A and Layer B fields are **optional** and gated by their respective `has*Texture` flags or `>0` checks. Existing non-skin scenes remain visually identical without changes.

### 6.2 `haircard`

| Field | Type | Default |
|-------|------|---------|
| `hair_color` | vec3 | `[1,1,1]` |
| `alpha_threshold` | float | `0.1` |
| `roughness` | float | `0.4` |
| `specular_intensity` | float | `0.05` |
| `specular_shift` | float | `0.1` |
| `color_variation` | float | `0.1` |
| `root_color` | vec3 | `[0.05,0.03,0.01]` |
| `tip_color` | vec3 | `[0.25,0.15,0.06]` |
| `hair_data_texture` | texref | (none) — RGB-packed mask/gradient/strand-id |
| `tangent_texture` | texref | (none) — RGB-encoded tangent-space flow |

### 6.3 `hairepic`

| Field | Type | Default |
|-------|------|---------|
| `tint_color` | vec3 | `[0.35,0.35,0.35]` |
| `thickness` | float | `0.002` |
| `roughness` | float | `0.4` |
| `specular` | float | `0.5` |
| `metallic` | float | `0.0` |
| `shift` | float | `5.2` (rad) |
| `ior` | float | `1.55` |
| `R`, `TT`, `TRT` | bool | `true` |
| `R_power`, `TT_power`, `TRT_power` | float | `1, 1, 2` |
| `eumelanine` | float | `0.6` |
| `pheomelanine` | float | `0.3` |
| `use_pigmentation` | bool | `false` |
| `use_scatter` | bool | `true` |
| `use_glints` | bool | `true` |
| `adv_shadows` | bool | `true` |
| `density_boost` | float | `1.0` |
| `scatter_boost` | float | `1.0` |
| `root_darkening` | float | `0.1` |
| `tip_bleaching` | float | `0.0` |
| `tip_falloff` | float | `8.0` |
| `variability` | float | `0.1` |

### 6.4 `hair`

| Field | Type | Default |
|-------|------|---------|
| `eumelanine` | float | `1.3` |
| `pheomelanine` | float | `0.2` |
| `thickness` | float | `0.003` |
| `density` | float | `0.7` |
| `roughness` | float | `8.5` (deg) |
| `az_roughness` | float | `0.35` |
| `shift` | float | `-5.2` (deg) |
| `ior` | float | `1.55` |
| `R`, `TT`, `TRT` | bool | `true` |
| `R_power`, `TT_power`, `TRT_power` | float | `4, 2, 4` |
| `use_scatter` | bool | `false` |
| `use_pigmentation` | bool | `true` |

### 6.5 `hairdisney`

| Field | Type | Default |
|-------|------|---------|
| `r_color`, `tt_color`, `trt_color` | vec3 | renderer defaults |
| `backscatter_color`, `frontscatter_color` | vec3 | renderer defaults |
| `r_intensity`, `tt_intensity`, `trt_intensity` | float | `1.0` |
| `glints_intensity`, `frontscatter_intensity`, `backscatter_intensity` | float | `1.0` |
| `thickness`, `density`, `roughness`, `az_roughness`, `shift`, `ior` | float | — |
| `R`, `TT`, `TRT` | bool | `true` |
| `use_scatter` | bool | `false` |
| `use_pigmentation` | bool | `true` |

### 6.6 `unlit`

| Field | Type | Default |
|-------|------|---------|
| `color` | vec4 | `[1,1,0.5,1]` (RGBA) |
| `color_texture` | texref | (none) |

### 6.7 Texture references (`texref`)

A `texref` is a JSON string:

- `"path/to/file.png"` — relative to `resourcesPath`; loaded via `Tools::Loaders::load_texture`. Color-channel textures use `TEXTURE_FORMAT_TYPE_COLOR`; normal/tangent maps use `TEXTURE_FORMAT_TYPE_NORMAL`.
- `"$GLB[N]"` — sentinel referring to the Nth embedded texture in the same mesh's GLB (only meaningful when the mesh `type` is `"glb"`). Out-of-range indexes log a warning and resolve to null.
- `null` or `""` — no texture.

---

## 7. `scene` (globals)

```json
"scene": {
  "ambient_color":     [0.05, 0.05, 0.05],
  "ambient_intensity": 0.1,
  "use_ibl":           true,
  "fog":   { "enabled": false, "color": [0.2,0.2,0.2], "intensity": 20.0 },
  "skybox": { "hdri": "textures/studio_demo.hdr", "intensity": 1.0, "rotation": 0.0, "blurriness": 0.0 }
}
```

All fields optional. `skybox.hdri` is required if `skybox` is present.
Setting a skybox implicitly enables IBL.

---

## 8. `renderer` (per-scene renderer hooks)

```json
"renderer": {
  "clear_color":     [0.0, 0.0, 0.0, 1.0],
  "sss_scatter_lut": "textures/scatterDistance.png"
}
```

| Field | Notes |
|-------|-------|
| `clear_color` | Returned in `LoadResult.clearColor`; the application is responsible for applying it during renderer construction (HairViewer sets it in `init()` before `setup()`; SLViewer does the same). |
| `sss_scatter_lut` | Path applied to `ForwardRenderer::load_sss_scatter_lut` if `renderer` is non-null. Deferred internally if the renderer hasn't initialized yet. |

---

## 9. Animation binding

Per-mesh `"animation"` is **optional**. Behavior:

- **HairViewer**: loads the scene's animation reference as-is. If no mesh declares one, no animation plays (rest pose).
- **SLViewer with positional `<animation.json>` arg**: that arg **overrides** the first mesh's `animation` field. If no mesh declares an animation, the first skinned mesh receives the override. If no mesh is animated or skinned, a warning is logged and the scene renders without animation.

Result lookup (`LoadResult.primaryAnimated`) returns the mesh that received the
animation, so SLViewer can derive the frame budget from the animation header.

---

## 10. Worked example

`resources/scenes/default.json` — the current Alex configuration:

```json
{
  "name": "alex_default",
  "camera": { "position": [0,0,-16], "near": 0.1, "far": 100, "fov": 40 },
  "lights": [{
    "type": "point", "name": "PointLight",
    "position": [-5,1,-5], "intensity": 1.0,
    "shadow_fov": 120, "shadow_bias": 0.0002, "shadow_near": 0.1,
    "area_of_effect": 30, "dummy_mesh": "sphere.obj"
  }],
  "meshes": [
    {
      "name": "Alex", "type": "glb", "file": "models/alex/alex.glb",
      "glb_mesh_index": 0,
      "position": [0,-12.6,0.2], "scale": 10, "rotation": [0,180,0],
      "material": {
        "type": "pbr", "albedo_texture": "$GLB[0]",
        "albedo": [0.8, 0.482, 0.333], "metalness": 0, "roughness": 0.5
      },
      "animation": "animations/test_anim.json"
    },
    {
      "name": "HairCards", "type": "obj",
      "file": "models/alex/hair_fauxmohawk.obj",
      "position": [0,-11.8,0.4], "scale": 0.1, "rotation": [0,180,0],
      "material": {
        "type": "haircard",
        "hair_color": [0.05,0.02,0.01],
        "hair_data_texture": "textures/alex/hair_fauxmohawk_attribute.png",
        "tangent_texture": "textures/alex/hair_fauxmohawk_tangent.png"
      }
    }
  ],
  "scene": {
    "ambient_color": [0.05,0.05,0.05], "ambient_intensity": 0.1,
    "skybox": { "hdri": "textures/studio_demo.hdr", "intensity": 1.0 },
    "use_ibl": true, "fog": { "enabled": false }
  },
  "renderer": {
    "clear_color": [0,0,0,1],
    "sss_scatter_lut": "textures/scatterDistance.png"
  }
}
```
