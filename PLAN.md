# Plan

Forward-looking work for this project. Completed features are tracked in git history; reference docs live in `CLAUDE.md`, `ANIMATION.md`, and `SCENE.md`.

---

## Open

### Renderer performance — opportunities for future work (notes 2026-06-24)

After fixing the host-visible-`posSSBO` regression (see Done, 2026-06-24), these are the remaining per-frame costs worth attacking, roughly highest-leverage first. None are started; each notes where it lives and the risk.

**CPU (per-frame, single-threaded — scales with vertex/strand count):**
1. **Hair positions are repacked + uploaded each animating frame.** `cycle_animatable_upload` builds a `Vec4` per vertex into `posStaging` (≈1.35M verts for maria) on top of the interleaved VBO upload, and the voxelization DMA-copies it to the device-local `posSSBO`. This is the live-voxel cost that keeps us a bit below develop2 when animating (see Done, 2026-06-24). To remove it: skin/deform on the **GPU** (compute) writing the packed device `posSSBO` directly — no CPU repack, no host→device copy. Largest payoff, largest effort; pairs with item 3.
2. **`apply_deformation` does two full vertex-array copies/frame** (`geometry.cpp:87` `deformed = vertexData`, then `:154` `deformedVertexData = deformed`). Reorder to upload first, then `deformedVertexData = std::move(deformed)` — saves one `N`-vertex copy per animating mesh per frame. Low risk; the binder reads `deformedVertexData` *after* the head's `apply_deformation` returns, so the move is safe.
3. **CPU skinning/morph + hair-binding reconstruction are scalar `for`-loops** (`apply_deformation`, `HairBinder::update`). For large grooms this dominates. Options, increasing effort: thread over strands/vertices (OpenMP/`std::for_each(par)`), SIMD the skin matrix×vertex, or move the whole deform+bind to a GPU compute pass (the CLAUDE.md hair-binding note already flags "GPU compute path is possible future work").

**GPU memory placement:**
4. **Animatable VBO is still `CPU_TO_GPU` (host-visible).** The hair `posSSBO` was moved to device-local (Done, 2026-06-24), but the VBO ring (~226 MB for maria) is still host-visible and read every frame as vertex input + as the bindless storage buffer for any consumer. Vertex fetch tolerates host-visible better than the voxelization march did, but if profiling shows the forward/shadow passes are PCIe-bound, give the VBO the same device-local + staging-ring treatment. Profile first.

**GPU shading (the bigger overall renderer cost):**
5. **Restored voxel cone-trace hair self-shadow** (`computeHairShadow` + `computeHairShadowCone` in `hair_strand_epic.glsl`) is the per-fragment shadow cost of the current hair look — several volume samples per light per fragment over the 256³ `HAIR_VOXEL_VOLUME`. The cheaper alternatives are already explored: the VSM-only path (`6c635e4`, fast but no hair-on-hair occlusion) and the abandoned Deep Opacity Map (see next entry). Revisit only once the look is locked.
6. **General forward-pass profiling** — the post-process chain (SSAO, SSS, bloom, DoF, tonemap, FXAA) runs full-res every frame; MSAA 8× resolves are not free. A GPU timestamp query per pass (the RHI already records command buffers per `IBasePass`) would show where the frame actually goes before guessing. Worth adding a debug timing readout before any GPU-side optimization.

### Hair self-shadow — Deep Opacity Map attempt (abandoned 2026-06-23, notes for future work)

**Goal.** Replace the voxel cone-trace hair self-shadow (which produced light-dependent
streak/wedge artifacts at 256³) with a directional, artifact-free hair-on-hair self-shadow,
to tame the backlit blonde glow without flattening front-lit colour. A non-directional
density "AO" was tried first and rejected (it dimmed equally in all directions → killed the
blonde, since inter-fiber multiple scattering *is* the blonde look). The directional
requirement led to a **Deep/Opacity Shadow Map** (Yuksel 2008).

**What was built (all reverted; reconstruct from this entry if needed).**
- New `HairDeepOpacityPass` (`core/passes/hair_deep_opacity_pass.{h,cpp}`), modeled on
  `VarianceShadowPass`: renders strand hair from each light's POV into an RGBA16F
  `sampler2DArray` (slice per light) via the same geometry-shader layer fan-out
  (`gl_Layer = lightId`, transform by `lights[i].viewProj`), additive blend, depth test off,
  hair-only (inverse of the VSM hair filter). Registered as `HAIR_DOM_PASS` (enum slot 3,
  shifting FORWARD_PASS→4 …) in `ForwardRenderer::create_passes`; its attachment fed to the
  forward pass via a `ForwardPass::set_hair_dom_descriptor()` setter (binding **15**) called
  once in `on_before_render` (NOT the dependency table — that hardcodes binding 2 for the VSM).
- Shader `resources/shaders/shadows/hair_dom.glsl` + sampling in `hair_strand_epic.glsl`.

**Hard-won findings (these are the value of this entry):**
1. **`read_file()` footgun:** a unified `.glsl` MUST start with a `#shader` directive — any
   line before the first directive is written to `ss[(int)StageType::NONE] == ss[-1]`, an
   out-of-bounds write that segfaults.
2. **`file(GLOB)` is configure-time:** a brand-new engine `.cpp` is not compiled until
   `cmake` is re-run (else link error `vtable ... undefined`).
3. **Depth parameterization is the whole battle.** NDC depth (`gl_FragCoord.z`) is
   perspective-compressed to ~1e-4 across the hair → layering collapses. Linear distance
   from the light works. Normalizing over the shared hair-union AABB compresses the scalp
   hair into a thin slice (eyebrows/lashes stretch the box) → use the **per-mesh** extent,
   packed into the unused `minCoord.w / maxCoord.w / otherParams1.w` lanes by
   `ResourceManager::update_object_data` (min/maxCoord.xyz are taken by the shared union for
   the voxel volume), projected onto the light direction so the depth box auto-sizes per
   light (no fixed-radius "cut" that slides with the light).
4. **Discrete layers always band.** 4 hard `int(t*4)` layers, even smooth-splatted, leave
   slope-kinks at the layer boundaries → a sharp light-relative "cut" line. The fix that
   worked: **moment-based** opacity — accumulate `sum(o), sum(o·t), sum(o·t²)`, reconstruct
   "opacity in front of depth t" with a smooth **logistic CDF** (mean/variance, variance
   floored by `MIN_STDDEV` as a softness knob). Continuous → no banding/cut.
5. **Sparse rasterization → acne.** Thin hair lines hit each shadow texel randomly; a single
   bilinear sample reads per-texel noise → self-shadow acne once opacity is low enough to see
   the colour. Mitigated (not eliminated) by a multi-tap blur of the (linear) moment map on read.
6. **Application / perceived inversion:** routing the DOM transmittance through the dual-
   scattering `hairCount` can *brighten* backlit hair (more layers → stronger forward-scatter
   `Tf·Sf`), reading as inverted self-shadow. Applying `T_dom` as a **direct multiplier on
   the light's contribution** (`color += lighting * T_dom`) darkens reliably.

**Why abandoned:** even after all the above, the opacity sat on a knife-edge — strong enough
to shadow looked too dark / re-introduced acne, weak enough to stay blonde did almost nothing
(`T_dom ≈ 1`). Reverted to the voxel cone-trace baseline at the user's request, keeping only
the per-light **distance attenuation** added on the hair (`computeAttenuation` in
`hair_strand_epic.glsl`) — a genuine win that fixed the runaway hair-vs-skin contrast when the
character moves far from the light (the hair direct term previously ignored distance falloff
while the skin's didn't).

**If resumed:** moment-based DOM (4) + per-mesh adaptive box (3) + direct-multiply
application (6) is the right skeleton; the remaining problem is the huge per-texel strand-
count dynamic range making the opacity un-tunable. Try: normalize opacity by per-texel
coverage, a Fourier opacity map, or a higher-res (512³) hair voxel volume for the cone-trace.

### Skin realism — authored maps + microdetail & pores

Two-layer push on skin realism. **Layer A** surfaces the authored map sets already shipped under `resources/textures/<character>/` — some slots exist in `PhysicallyBasedMaterial` (albedo/normal/roughness/AO/metallic/emissive), others don't (bent normal, curvature, scattering, clothes mask) and need engine + shader work. **Layer B** adds pore-scale detail on top of the base normal — mostly inside `physically_based.glsl`, with a small cavity-aware modulation in `ssss.glsl`. No new render passes, no C++ pass restructure.

**Layer B technique selection** (highest realism-per-effort; Fresnel-in-cavity and detail-roughness perturbation were considered and skipped as marginal):

1. *Detail (pore) normal map* — tiled high-frequency tangent-space normal blended with base normal. Tiling + strength configurable per material.
2. *Cavity map + specular occlusion* — grayscale cavity attenuates specular only, suppressing "shiny pore" artifacts. Same map feeds `ssss.glsl` to shrink scatter weight in crevices.
3. *Dual-lobe specular* — two GGX lobes (sharp ~0.35 + soft ~0.55) mixed for layered oily/dry skin highlight (Penner GDC 2011).

**Assets required from user (for Layer B):**

- `resources/textures/skin/pore_normal.png` — tileable RGB tangent-space normal (Y-up, same convention as the engine's other normal maps).
- `resources/textures/skin/pore_cavity.png` — tileable grayscale cavity aligned with `pore_normal` (white = flat, dark = pore).

Both should come from the same source asset so cavities align with normal-map dips. Free sources: Poly Haven "skin pores", Textures.com tileable skin detail. Per-character overrides can be added later via scene JSON.

**Schema additions** (`pbr` material in `SCENE.md`, all optional):

- Layer A: `bent_normal_texture`, `curvature_texture`, `scattering_texture`, `clothes_mask_texture` (each a path or `$GLB[N]`).
- Layer B: `detail_normal_texture`, `detail_cavity_texture`, `detail_tiling` (float, default `8.0`), `detail_normal_strength` (float `[0,1]`, default `0.5`), `cavity_spec_occlusion` (float `[0,1]`, default `1.0`), `dual_lobe_mix` (float `[0,1]`, default `0.15`), `dual_lobe_roughness_soft` (float, default `0.55`). _(`cavity_sss_attenuation` was originally part of this set; rolled back on 2026-06-03 — see Done entries.)_

**Implementation order** (each step validated before the next):

1. ~~Maria authored maps wiring (Layer A, no engine change).~~ **Done — see `## Done` 2026-06-02.**
2. ~~Engine extension for the remaining 4 maps (Layer A): slots, descriptor layout, samplers, scene_loader.~~ **Done — see `## Done` 2026-06-02.**
3. ~~Curvature → pre-integrated skin diffuse (Penner 2011).~~ **Done — see `## Done` 2026-06-02.**
4. ~~Bent normal → diffuse IBL direction.~~ **Done — see `## Done` 2026-06-02.**
5. ~~Scattering map → per-texel SSS modulation.~~ **Done — see `## Done` 2026-06-02.**
6. ~~Clothes mask → suppress skin SSS / pre-integrated / bent-normal on clothing.~~ **Done — see `## Done` 2026-06-02.**
7. ~~*(User asset drop)* `normal_detail.png` + `cavity_detail.png` in `resources/textures/skin/`.~~ **Done — assets in place.**
8. ~~Detail (pore) normal blended into forward shader.~~ **Done — see `## Done` 2026-06-02.**
9. ~~Cavity map + specular occlusion (Layer B).~~ **Done — see `## Done` 2026-06-02.**
10. ~~Dual-lobe specular (Layer B).~~ **Done — see `## Done` 2026-06-02.**
11. ~~Cavity-aware SSS in `ssss.glsl` (Layer B).~~ **Done — see `## Done` 2026-06-02.**
12. ~~Scene schema + docs.~~ **Done — see `## Done` 2026-06-02.**
13. ~~Regression sweep (uncovered + fixed a pre-existing engine offset bug).~~ **Done — see `## Done` 2026-06-02.** Debug `HairViewer --frames 10 --log-level warn` clean across alex/javi/maria/nadia. Confirm hair, eyes, and non-skin materials unaffected.

As each step completes, move the relevant write-up into `## Done` with a date (matching the existing entries) and record issues found + fixes shipped + verification.

---

### Hair-to-scalp surface binding (`.hair`)

Attach `.hair` assets (scalp hair, eyebrows, eyelashes) to the character head so they (1) don't clip into the skin, (2) don't float above it, (3) follow face **morph-target** animation (not just the head bone), and (4) keep their overall silhouette. Scope: `.hair` only — the neural `.ply` path is deprecated/unused.

**Core idea — bind in head-local space, drop the joint attachment.** The head's deformed vertex positions are computed CPU-side every frame (`Geometry::apply_deformation`, `geometry.cpp:83`), so we bind hair to the head *surface* in head-local space instead of riding the `"head"` bone via `JointAttachment`. The bound hair inherits the head's base model matrix; all skeletal **and** morph motion flows in through the deformed surface verts. The head's untouched `vertexData` is always the rest pose (deformation runs on a copy), so it's available for binding for free.

**Technique — barycentric groom binding.** Per strand root: nearest head triangle + barycentric `(u,v)` + a bind-pose root frame (interpolated normal + tangent basis); strand points stored in that frame. Static fit (snap root to surface, small normal epsilon) solves clip/float. Per-frame: rebuild the root frame from the *deformed* triangle and apply the rigid delta `T = M_frame_deformed · M_frame_bind⁻¹` to the strand (rigid-per-strand preserves silhouette). Static head → computed once.

`.hair` layout (`loaders.cpp:1118-1182`): vertices are strand-contiguous, `segments[i]+1` verts per strand, root = first vert of each strand.

**Decisions (locked):** grossly-misaligned assets → interactive gross alignment needed; in-engine bind mode writing a per-asset sidecar; CPU per-frame deform (compute-pass optimization deferred); alignment UI = transform sliders + live preview (3D gizmo deferred).

**Implementation order** (each phase verified with `HairViewer --frames N --log-level warn`, then a manual visual check):

1. ~~*Phase 0 — plumbing.*~~ **Done.** Retained deformed CPU vertex buffer on `Geometry` (+ `get_deformed_vertices`); per-strand `strandOffsets` captured in `load_hair`; opt-in `set_animatable` flag wired into the upload/BLAS decision (`.hair` marked animatable in `load_hair`). Added `upload_vertices` + `update_bounds`.
2. ~~*Phase 1 — binder core.*~~ **Done.** `src/hair_binding.{h,cpp}` — `HairBinder`: nearest-triangle projection (brute force, one-time), bary + bind-pose root frame, strand points in local frame, root snap + tangent-plane **declip**. Verified: shadow proved roots land on the scalp.
3. *Phase 2 — runtime follow (CPU).* **Implemented, not yet visually validated** — `update()` rebuilds the frame from the deformed head surface and rigidly transforms each strand; needs a morph/skeletal animation to confirm follow (user deferred wiring a test anim).
4. ~~*Phase 3 — bind mode + sidecar.*~~ **Done.** `HairBindWidget` panel (align sliders, normal offset, declip toggle, Bind/Save/Load + bind report). `.hbnd` binary sidecar (auto-loaded next to asset). Scene schema `"bind_to"` / `"binding"` → `LoadResult::hairBindings`; app builds binders (kept out of SLViewer's link). Auto-detect fallback when no `bind_to`.
5. *Phase 4 — roll out + docs.* **Docs done** (`CLAUDE.md` "Hair-to-Scalp Surface Binding", `SCENE.md` `bind_to`/`binding`). **Remaining:** validate Phase 2 follow under animation; consider full surface-collision declip for long drapes; SLViewer headless binder support; then move this entry to `## Done`.

**Known issues / notes:**
- Declip is a tangent-plane clamp (per root triangle) — short hairs good, long drapes far from their root may still clip.
- SLViewer build currently fails at a **pre-existing** embedded-shader codegen error (`evalEpicHairBSDF` overload in `forward_fast_hair_strand_epic.glsl`), unrelated to this work; SLViewer also doesn't drive binders yet.

---

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

### Performance regression from *Fixed shadows on hair* — host-visible hair `posSSBO` (2026-06-24)

FPS dropped from ~35-40 to ~30 after *Fixed shadows on hair* (`6c635e4`); verified by removing that whole commit on a `develop2` branch (→ 35-40 again). The commit's only **net-new per-frame GPU cost** was the position-SSBO change (the `2026-06-19` entry): it moved hair's `posSSBO` from device-local (`GPU_ONLY`, uploaded once) to a host-visible (`CPU_TO_GPU`) ring updated every frame. (Its shader change removed the voxel cone-trace, but that was later reverted, so per-fragment shading matches the `65820d4` baseline — the regression is the buffer, not shading.)

**Root cause:** maria hair is **1.35M vertices**. The animatable VBO ring (~226 MB) **plus** the new host-visible `posSSBO` ring (~65 MB) overflow a non-ReBAR 256 MB BAR, so the `posSSBO` lands in system RAM and `HAIR_VOXELIZATION_PASS` (`DDA_fiber_optical_density.glsl`) marches ~22 MB of strand positions across **PCIe every frame** — present even in a static pose, since the voxelization runs every frame. `develop2` is faster only because its `posSSBO` is device-local (frozen). A first attempt (gating the host-visible ring to hair only, sparing the head) did **not** help: the head's mirror was pure waste, but the *hair* posSSBO — the one actually read by the march — was still host-visible.

**Fix shipped — device-local `posSSBO` + per-frame staging→device copy** (industry-standard pattern for GPU-read-heavy dynamic data; keeps the voxel self-shadow live, unlike `develop2`'s freeze):
- `vao.h` — `posSSBO` is now **always device-local** (`GPU_ONLY`, bound at offset 0 / full size). Live-deformed hair adds `posStaging` (host-visible `CPU_TO_GPU` ring) + flags `posLiveCopy` / `posStagingDirty`.
- `device.cpp` (`upload_vertex_arrays`, gated by the `livePositionSSBO = gd.forceAnimatable` param) — hair allocates device-local `posSSBO` (TRANSFER_DST) + `posStaging` ring (TRANSFER_SRC), seeds both. Static / animatable-not-live geometry (the head) keeps the single device-local upload-once path.
- `geometry.cpp` (`cycle_animatable_upload`) — writes the repacked `Vec4` positions into `posStaging` and sets `posStagingDirty`.
- `command_buffer.{h,cpp}` — new `copy_buffer(src,dst,size,srcOff,dstOff)`.
- `hair_voxelization_pass.cpp` (`render`) — for each live hair geom with `posStagingDirty`: **WAR barrier** (prior reads finish) → `copy_buffer(posStaging[frameOffset] → posSSBO)` → **RAW barrier** (this frame's reads see fresh data), then clear the flag. Skipped when not dirty, so a paused pose (binder early-outs) costs nothing → full `develop2` speed. Recorded outside any renderpass (compute path).
- `forward_pass.cpp` / `hair_voxelization_pass.cpp` (`update_uniforms`) — `posSSBO` descriptor writes reverted to `(size, offset 0)` (no per-frame `readOffset`; the device buffer is always current after the copy).

**Net effect:** the heavy voxelization march reads fast VRAM again. When animating, a ~22 MB DMA copy + the CPU `Vec4` repack remain (so a touch below `develop2` — that's the cost of keeping the voxel self-shadow live; eliminating it needs GPU skinning, see Open item 1). When paused, no copy → `develop2` speed.

**Verified:** clean build; `--frames 40 --log-level warn` with the dance animation playing → EXIT 0, **0** validation errors/hazards (the per-frame copy + WAR/RAW barriers are exercised and clean). SLViewer shares this engine code (picks it up on its next build). **Pending user FPS confirmation** under animation.

### Dark shadow streaks / wedges on the hair (point-light dependent) (2026-06-23)

The strand hair showed dark, light-direction-dependent streaks and hard wedges — strongest at grazing light, and gone when the PointLight was disabled. Diagnosed entirely by GUI isolation + user screenshots (the dev environment can't render: Vulkan-over-VNC isn't screenshot-able and SLViewer crashes at init on the local NVIDIA driver). Two **independent** self-shadow sources on the hair, both fed by the point light:

1. **Voxel cone-trace self-shadow** (`hairShadow` / `computeHairShadowCone` in `hair_strand_epic.glsl`) — cone-marches the 256³ `HAIR_VOXEL` density. Over a large, scene-scaled groom the path integral through the coarse voxelized strands projects as hard streaks/wedges. Bias (normal-offset), mip-floor blur, and `FIBER_SCALE` reduction each only *partly* helped — confirmed not fully fixable at this voxel resolution.
2. **VSM shadow-map self-shadow** — strand hair was rendered into the variance shadow map *as lines* (`vsm_line_geom`), so when the hair sampled the map back (`solidOcclusion = computeVarianceShadow`) its own strands shadowed it → thin line acne. This is why disabling the shadow-map term *alone* didn't clean it (voxel still on) and disabling *both* did.

**Fix shipped** (clean hair, head→hair shadow preserved):
- `hair_strand_epic.glsl` — hair occlusion (both `directFraction` and the multiple-scattering `transMask.visibility`) now uses the smooth VSM `solidOcclusion` **only**; the voxel cone-trace self-shadow is no longer used on the hair. `computeHairShadow`/`hairShadow`/`computeHairShadowCone` are left defined but uncalled (reverted to their original bodies). A block comment records why and that re-enabling needs a higher-res hair voxel volume (e.g. 512³) / tighter per-mesh AABB.
- `variance_shadow_pass.cpp` — skip `HAIR_STR_*_TYPE` meshes when rendering the shadow map, so hair no longer self-shadows via the VSM (head/body still cast → head→hair shadow kept). Trade-off: hair no longer casts a shadow-map shadow onto the face/body (future work: VSM receiver-bias tuning to keep hair→skin shadow without hair-on-hair acne).

**Verified:** user confirmed "the hair looks clean" (PointLight on, HIGH shadows, the previously-worst back/crown grazing views). Earlier `posSSBO`/mip/bias/`FIBER_SCALE` experiments on the voxel path were reverted (dead code once the voxel self-shadow was dropped); the `posSSBO` ring itself is kept for SSAO/SSR.

### Hair self-shadow / scattering frozen under animation — stale `posSSBO` (2026-06-19)

With a moving animation (e.g. `dance_anim.json`) the strand hair rendered and moved correctly, but its volumetric self-shadow / scattering stayed frozen at the groom pose. Root cause: the hair's density comes from `HAIR_VOXELIZATION_PASS` (active mode `OPTICAL_DENSITY == 1`), whose compute shader (`DDA_fiber_optical_density.glsl`) reads strand positions from the bindless **`posSSBO`** and transforms them by the model matrix. `posSSBO` was filled **once at load** (`device.cpp`, GPU-only) and never refreshed. Surface-bound hair animates by the binder rewriting *vertices* every frame (`HairBinder::update → upload_vertices`, which ring-updates `vao.vbo` only) while the strand model matrix stays ~identity — so the voxel density was built from the original positions and froze. Same bug class as the earlier flicker fix (a GPU buffer not following the per-frame CPU deformation); that fix updated the VBO but missed `posSSBO`. `posSSBO` is also read by SSAO and SSR, so those were stale on hair too.

**Fix shipped:** mirror the VBO ring-buffer onto `posSSBO`, no GLSL changes.
- `vao.h` — `posCopies` / `posCopyStride` / `posFrameOffset`, cycled in lockstep with `vboWriteIndex`.
- `device.cpp` (`upload_vertex_arrays`) — animatable geometry allocates `posSSBO` host-visible (`CPU_TO_GPU`), `RING×` sized, region stride aligned to `minStorageBufferOffsetAlignment`, region 0 seeded. Static meshes keep the GPU-only single-region staging path.
- `geometry.cpp` (`cycle_animatable_upload`) — extract `Vec4` positions from the deformed vertices into the matching `posSSBO` ring region (reused `m_posUploadScratch`), record `posFrameOffset`.
- `forward_pass.cpp` + `hair_voxelization_pass.cpp` (`update_uniforms`, run per-frame) — write the bindless `posSSBO` descriptor with `readOffset = posFrameOffset` and range = one region for animatable geometry (offset 0 / whole buffer for static).

**Note (2026-06-23):** this was a real latent fix (the voxel density now tracks the animation, and `posSSBO` was also stale for SSAO/SSR) but it was **not** the artifact the user was actually seeing — see the next entry. The kept value of this change is the SSAO/SSR correctness; the hair's voxel self-shadow that it fed is now disabled on the hair (artifacts), so its effect on hair shading is moot until the voxel volume is re-enabled at higher resolution.

Keeps the no-race guarantee (GPU reads frame N's region while the CPU writes frame N+1's disjoint region — same discipline as the flicker fix). `RING == 3` reused (correct for DOUBLE buffering; bump to 4 if the renderer goes TRIPLE, same note as the VBO ring).

**Verified:** Debug `--frames` harness adds **0** new validation errors vs. the stashed baseline (both show the same 182 pre-existing image-clear WAW hazards in the voxelization pass clears, unrelated to this change; 0 buffer hazards). **Pending user visual confirmation** that the hair shadow now tracks `dance_anim.json` in HairViewer.

### SLViewer exports video with no hair — binders not driven headless (2026-06-18)

`SLViewer` produced videos with the character bald, while HairViewer (same scene) showed the hair. The `.hair` meshes *were* in the scene (the loader adds them and records `bind_to` requests in `result.hairBindings`), but `SLApplication::setup()` ignored `hairBindings` — no `HairBinder` was built and `tick()` had no `binder->update()`. So the strand hair stayed at its raw, unbound groom-space coordinates (grossly misaligned / off-camera) and never appeared in the framed video. `hair_binding.cpp` was historically not even linked into SLViewer.

**Fix shipped:** port the hair surface-binding pipeline into SLViewer, reusing the exact same module HairViewer uses.
- `CMakeLists.txt` — add `src/hair_binding.{cpp,h}` to `SLVIEWER_SOURCES`/`SLVIEWER_HEADERS`.
- `src/slviewer/application_sl.{h,cpp}` — `m_binders` + a `setup_hair_binding()` that mirrors HairViewer (explicit `bind_to` requests, else auto-discover head = first skin/morph mesh and bind every `.hair`; auto-loads the `<hair file>.hbnd` sidecar via `make_binder`). Called from `setup()` after `load_scene_json`; `tick()` runs `binder->update()` for each binder before `render()`.

**Verified:** user confirmed the headless export now renders the hair on the scalp. CLAUDE.md updated (removed the "SLViewer does not drive the binders / hair_binding not linked into SLViewer" limitations).

### Flickering black "strand" lines on animated skin — VBO race under frames-in-flight (2026-06-18)

With `test_anim.json` playing, thin dark flickering lines appeared on the **animated** skin (right arm, face) and nowhere else. They tracked four conditions: present only on animated geometry, present at HIGH shadow quality (gone at lower), present when hair cast shadows, and — the confusing one — they **disappeared when several hair meshes were loaded**.

**Misdirections (ruled out, but left useful changes):** First suspected a missing shadow-map read barrier, then self-shadow acne. Enabling **synchronization validation** (temporarily, via `VkValidationFeaturesEXT` in `bootstrap.cpp`) surfaced real cross-frame `WRITE_AFTER_WRITE` hazards on the single-buffered **shadow depth** and **forward depth** attachments. Serializing those (added barriers in `variance_shadow_pass.cpp` and `forward_pass.cpp`, plus an `aspect` param on `CommandBuffer::pipeline_barrier`) eliminated the hazards (56→0) **but not the visible lines** — so they were real bugs, just not this one. A freeze-pose toggle (P) showed the lines **vanish when the pose is frozen**, and a forced `vkDeviceWaitIdle`-before-deform toggle showed them **vanish with animation still playing** → a CPU/GPU data race, confirmed.

**Root cause:** `Geometry::apply_deformation` / `upload_vertices` re-`memcpy` the deformed vertices into a **single host-mapped VBO** every frame (`geometry.cpp`), but the renderer runs `BufferingType::DOUBLE` (2 frames in flight) with **no per-frame copy and no fence gating the write**. The CPU overwrote the VBO while the GPU was still reading it for an in-flight frame — and even within one frame the shadow pass and forward pass could fetch different poses → the body's depth no longer matched its own shadow map → flickering self-shadow lines on exactly the vertices that moved. Invisible to sync-validation because it's a host `memcpy` racing GPU reads on persistently-mapped memory, not a GPU-command hazard. ("More hair meshes → gone" was just extra GPU work shifting the race window.)

**Fix shipped:** double-buffer animatable VBOs. The VBO now allocates `RING = 3` (≥ frames-in-flight + 1) back-to-back copies of the vertex data; each re-upload advances a ring cursor, writes a different region, and records its byte offset, which the draw binds. CPU writes and GPU reads never touch the same region, and the shadow/forward passes read the same region within a frame.
- `ext/Vulkan-Engine/include/engine/graphics/vao.h` — `vboCopies`/`vboCopyStride`/`vboWriteIndex`/`vboFrameOffset` on `VertexArrays`.
- `ext/Vulkan-Engine/src/graphics/device.cpp::upload_vertex_arrays` — allocate `vboSize * RING` for animatable VBOs (seed region 0).
- `ext/Vulkan-Engine/src/core/geometries/geometry.cpp` — `cycle_animatable_upload()` ring cursor, used by `apply_deformation` (skin/morph) and `upload_vertices` (surface-bound hair).
- `ext/Vulkan-Engine/src/graphics/command_buffer.cpp::draw_geometry` — bind `vao.vboFrameOffset` (0 for static geometry).
- **Note:** `RING = 3` assumes DOUBLE buffering; raise to `framesInFlight + 1` (4) if the renderer is ever switched to `TRIPLE`.

Also added a runtime **freeze-animation toggle (key P)** in `HairViewer` (kept as a feature). **Verified:** user confirmed the lines are gone with animation at full speed.

### d'Eon hybrid normals + SSS cavity rollback (2026-06-03)

Two related refinements to the skin pipeline, validated together by the user against side-by-side renders of Maria.

**(1) Diffuse/specular normal split with d'Eon hybrid normals.** The detail-normal map was previously sampled once at LOD 0 and used by both diffuse and specular, so pore-scale bumps survived into the diffuse term. The screen-space SSS blur cannot un-bake high-frequency normal variation already shaded into per-pixel color, so the skin read as slightly "dry / 3D-printed" even with strong SSS. Implemented the Penner GDC 2011 + d'Eon/Hanrahan SIGGRAPH 2007 hybrid: specular still samples the sharp detail normal, while each RGB channel of the diffuse term integrates against a per-channel *pre-blurred* detail normal — red widest, blue sharpest — mimicking wavelength-dependent subsurface scattering at pore scale. The per-channel mip-LOD biases are derived in-shader from the SSS scatter-distance LUT (e.g. `monk05.png` for Maria) so the spectral profile of the hybrid normals always matches the SSS pass's profile, and so changing the LUT per character automatically retunes the hybrid normals.

**(2) Cavity-attenuated SSS rolled back.** Step 11 of the original Skin realism initiative wired the cavity map into the SSS kernel via `outDiffuseIrr.a = skinMask * mix(1.0, cavity, cavity_sss_attenuation)`. Visual comparison showed this over-attenuated scatter inside pore crevices and broke the spectral smoothness real skin shows — light physically enters and scatters laterally even at the bottom of a pore. Cavity occlusion now lives only in the specular path (`cavity_spec_occlusion`), where the "no shiny pores" effect belongs. The `outDiffuseIrr.a` channel still carries `skinMask` for the SSS early-out and for the `modulatedDiff` / `modulatedSS` lerp; the per-tap multiplication by the alpha has been removed.

**Changes shipped:**

- `ext/Vulkan-Engine/include/engine/core/passes/sss_pass.h` — exposed `get_scatter_lut_texture()` on `SSSPass` so other passes can sample the same LUT image.
- `ext/Vulkan-Engine/src/core/passes/forward_pass.cpp` + `forward_pass.h` — added binding 14 (`scatterDistLUT`) to `GLOBAL_LAYOUT` of the forward pass, initialized to the fallback texture, with a `set_scatter_lut_descriptor(Image)` setter so the scene loader can install the real LUT post-construction.
- `ext/Vulkan-Engine/include/engine/systems/renderers/forward.h` — `load_sss_scatter_lut` now also forwards the LUT image to the forward pass, keeping both pipelines in sync.
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — added `scatterDistLUT` sampler at set=0 binding 14; `setupBRDFProperties` now builds three world-space "diffuse normals" (`diffuseNormalWS_R/G/B`) via `textureLod(detailNormalTex, dUV, bias_c)` with biases derived as `clamp(1.5 * log2(d/d_min), 0, 4)` from the LUT's mid-thickness pixel; the per-light loop's diffuse term became `vec3 NdotL_diff_rgb` consumed by both the lighting recomposition and the SSS irradiance write; `brdf.normal` stays sharp (LOD 0) for specular and the bent-normal/back-light paths; `smoothNormalWS` (base only, no detail) feeds the curvature wrap (Penner pre-integrated diffuse).
- `ext/Vulkan-Engine/resources/shaders/misc/ssss.glsl` — per-sample loop no longer reads `diffuseIrrTex.a` (skin gate still consumed at the early-out and final combine, just not folded into the per-tap diffusion weight).
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — `outDiffuseIrr.a` write reduced to plain `skinMask` (cavity factor removed).
- `ext/Vulkan-Engine/include/engine/core/materials/physically_based.h` + `.cpp` — removed `m_cavitySSSAttenuation` and its getter/setter; `dataSlot10.y` repurposed as padding (writes `0.0f`).
- `ext/Vulkan-Engine/include/engine/graphics/uniforms.h` — no shape change; the `slot11` field added mid-iteration for explicit per-material blur biases was removed once the LUT-derived path landed.
- `src/scene_loader.cpp` — removed JSON parsing of `cavity_sss_attenuation` and its `warn_unknown` entry; per-material `detail_blur_r/g/b` keys were never exposed (LUT-derived instead).
- `resources/scenes/maria.json` — dropped `cavity_sss_attenuation`. (alex/javi/nadia already lacked it.)

**Verified.** User compared two renders of Maria — diffuse-only path with vs. without the cavity multiplier in SSS, plus the hybrid-normals on/off. Confirmed the smoother form (no per-tap alpha gate) closely matches Activision/Weta digital-human references, with pore detail still legible in specular highlights on nose tip / cheekbone.

**Docs.** `SCENE.md` updated to drop `cavity_sss_attenuation` from the Layer B table and to clarify `detail_cavity_texture` is now spec-only. The `## Open` schema additions in this file also patched to mark the param as rolled back.

---

### Skin realism — regression sweep + latent uniform-buffer offset bug fix (step 13) (2026-06-02)

The skin realism initiative was visually verified on Maria, but the regression sweep against alex's scene (`default.json`, no skin maps bound) revealed every non-Maria scene rendering with extreme blocky color corruption across the PBR mesh. Diagnostic bisection ruled out the new shader code paths (gated by `has*Texture` flags that are all false for alex). The culprit was a **latent engine bug** that grew load-bearing only when `MaterialUniforms` got bigger.

**The bug.** Every site that places `MaterialUniforms` into the per-frame OBJECT layout buffer was using `pad_uniform_buffer_size(sizeof(MaterialUniforms))` as the **offset within the mesh's stride slot** where the material data starts. The correct offset is `pad_uniform_buffer_size(sizeof(ObjectUniforms))` — material data sits right after the object data, so the offset is the size of the object portion, not the size of the material portion. Both happened to equal the same value (128 or 256 depending on GPU UBO alignment) when `MaterialUniforms` was 8 Vec4 slots. As soon as it grew to 10 slots (160 bytes), the two padded sizes diverged, and every mesh's material reads bled past its own slot into the *next* mesh's slot — explaining why Maria (single PBR mesh) survived (her material reads adjacent buffer space that happens to be skybox or hair data, but the visual result on the central mesh was OK enough), while Alex with a HairCard mesh laid out next to the PBR mesh produced wild corruption.

**Why this was masked for years.** `pad(128) == pad(256)` on essentially every common GPU because UBO min-alignment rounds both up to 256. The bug only manifests when `sizeof(MaterialUniforms)` crosses an alignment boundary the way ours did. The skin realism initiative was the first growth past 128 bytes.

**Fix shipped** — replaced `pad(MaterialUniforms)` with `pad(ObjectUniforms)` at all 7 sites that compute the material offset within the per-mesh stride:

- `ext/Vulkan-Engine/src/core/resource_manager.cpp:339` (CPU-side upload offset)
- `ext/Vulkan-Engine/src/core/passes/forward_pass.cpp:239` (forward pass descriptor write)
- `ext/Vulkan-Engine/src/core/passes/shadow_pass.cpp:86` (shadow pass descriptor write)
- `ext/Vulkan-Engine/src/core/passes/variance_shadow_pass.cpp:76` (VSM descriptor write)
- `ext/Vulkan-Engine/src/core/passes/geometry_pass.cpp:148` (deferred-pipeline geometry pass descriptor write)
- `ext/Vulkan-Engine/src/core/passes/hair_scattering_pass.cpp:145` (hair LUT compute pass descriptor write)
- `ext/Vulkan-Engine/src/core/passes/hair_voxelization_pass.cpp:172` (hair voxelization compute pass descriptor write)

The stride computation in `ext/Vulkan-Engine/src/systems/renderers/renderer.cpp:275` (`objectStrideSize = pad(ObjectUniforms) + pad(MaterialUniforms)`) was already correct — it's adding both sizes to compute total per-mesh footprint, not an offset.

**Verified.** Alex (default.json) renders correctly. Maria (maria.json, all skin maps) continues to render correctly. User confirmed "the rest of the scenes" also rendering correctly.

**Investigation trail.** Initial false lead: my step 8 normal-blend refactor going through `v_TBN` for materials without normal maps — patched to fall back to `v_normal` directly when neither a base normal map nor a detail normal map is bound. That patch is kept (it removes a real risk of `normalize(zero)` NaN on tangent-less meshes), but it was not the cause of the alex regression.

---

### Skin realism — SCENE.md schema documentation (Layer A + B, step 12) (2026-06-02)

Documented all 12 new `pbr` material fields added by the Skin realism initiative. Two new subsections under `### 6.1 pbr`: one for Layer A (authored maps — bent normal, curvature, scattering, clothes mask) and one for Layer B (microdetail — pore normal/cavity textures plus the 6 tuning scalars). Each field documents its purpose in one sentence so future material authors know *why* it's there, not just *what* it accepts. All new fields are optional with explicit defaults; non-skin scenes remain visually identical without any JSON changes.

**Not done** (intentionally): rolling the maps into alex/javi/nadia scenes. Those characters ship only `<name>.png` (base color) + hair maps under `resources/textures/<name>/` — no authored normal/AO/roughness/etc. The wiring is parked until those assets exist.

**Not done** (intentionally): `CLAUDE.md` updates. The architecture overview (forward pipeline pass table, post-process pass authoring guide, scene format pointer, material-class summary) is still accurate after this work — the additions are all material-internal and surface through the `pbr` schema in `SCENE.md`. Nothing in `CLAUDE.md` describes the previous field list, so nothing is stale.

---

### Cavity-aware SSS (Layer B step 11) (2026-06-02)

The cavity map now also shapes the screen-space SSS blur so light no longer bleeds across pore boundaries. Implemented by repurposing the unused `outDiffuseIrr.a` channel as a **per-pixel SSS sample weight**, baked at forward-pass time. The SSS pass multiplies that weight into each kernel tap's diffusion contribution (both numerator and denominator) so pore crevices contribute proportionally less to their neighbours' scatter.

**Approach.** Avoided binding the cavity texture in the SSS pass — it's a global post-process, doesn't know which material a pixel belongs to. Instead the forward PBR shader writes `outDiffuseIrr.a = skinMask * mix(1.0, cavity, cavity_sss_attenuation)`. Every other shader (hair epic/strand/card/disney, unlit, phong, skybox) already writes `outDiffuseIrr.a = 0` — free benefit: those pixels are now naturally excluded from the SSS blur, which cleans up the hairline edge and silhouette transitions where hair/sky used to contribute to skin scatter via texture filtering.

**Changes shipped:**

- `ext/Vulkan-Engine/include/engine/core/materials/physically_based.h` — added `m_cavitySSSAttenuation` (default `0.0`) with getter/setter.
- `ext/Vulkan-Engine/src/core/materials/physically_based.cpp` — packed into `dataSlot10.y`.
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — writes the modulated weight into `outDiffuseIrr.a`. The slot10.y field (`cavitySSSAttenuation`) was already declared in the uniform block from step 9.
- `ext/Vulkan-Engine/resources/shaders/misc/ssss.glsl` — per-sample blur loop now reads `diffuseIrr.a` and folds it into the diffusion weight for both `scatteredIrr` and `totalWeight` accumulation; the normalize step is unchanged, so a region where many samples have low weight still produces a correctly-averaged result.
- `src/scene_loader.cpp` — parses `cavity_sss_attenuation`.
- `resources/scenes/maria.json` — final tuning landed at `cavity_sss_attenuation: 0.4`, `cavity_spec_occlusion: 0.5` (slightly softer than my initial 0.6/0.7), `detail_normal_strength: 0.4` (down from 1.0).

**Verified.** User confirmed cleaner pore-scale detail (boundaries no longer washed away by SSS), cleaner hairline/clothes edges (non-skin pixels no longer contributing).

---

### Dual-lobe specular (Layer B step 10) (2026-06-02)

Penner GDC 2011: skin's specular response is layered — a sharp oily peak on top of a broader dry-skin reflection. Approximated by mixing two GGX lobes at different roughnesses.

**Approach.** In the per-light loop, when `dualLobeMix * skinMask > 0`, copy the `SchlickSmithBRDF` struct, override only `.roughness` with `dualLobeRoughnessSoft`, re-evaluate the BRDF for the soft lobe, subtract the (unchanged) `diffBase` to isolate the soft specular, and `mix(specSharp, specSoft, effectiveDualMix)`. Order matters: dual-lobe is applied *before* cavity occlusion, so cavity attenuates the final dual-lobe specular. The diffuse-term `kD` depends on Fresnel, not roughness, so both BRDF calls share `diffBase` — no duplicated diffuse evaluation.

**Cost.** One extra BRDF call per light on skin pixels. Acceptable for face-filling renders. Gated by `skinMask` so clothes keep their single sharp lobe.

**Changes shipped:**

- `ext/Vulkan-Engine/include/engine/core/materials/physically_based.h` — added `m_dualLobeMix` (default `0.0` = off), `m_dualLobeRoughnessSoft` (default `0.55`) with getters/setters.
- `ext/Vulkan-Engine/src/core/materials/physically_based.cpp` — packed into `dataSlot10.zw`.
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — second BRDF call + mix in the per-light loop, gated by `effectiveDualMix > 0`.
- `src/scene_loader.cpp` — parses `dual_lobe_mix`, `dual_lobe_roughness_soft`.
- `resources/scenes/maria.json` — `dual_lobe_mix: 0.15`, `dual_lobe_roughness_soft: 0.55`.

**Verified.** User confirmed the layered sharp+soft highlight on cheekbone/nose under directional light.

---

### Cavity map + specular occlusion (Layer B step 9) (2026-06-02)

Pore crevices shouldn't glint — micro-occlusion should attenuate the specular term so the per-pore highlight from step 8's detail normal doesn't read as plastic. Tied to the same `detailTiling` as the detail normal so cavity dips align with normal-map indentations.

**Approach.** Refactored the per-light loop to hoist the diffuse/specular split (compute `F`, `kD`, `diffBase` once per light, derive `specPart = lighting - diffBase`). That split is then reused by both step 3's curvature wrap and step 9's cavity occlusion — cheaper than two independent Fresnel evaluations. Cavity occlusion multiplies only `specPart` by `mix(1.0, cavity, cavity_spec_occlusion)`; diffuse and SSS irradiance are untouched.

**Slot restructuring.** Moved `hasDetailCavityTexture` flag into `dataSlot9.w` (previously padding); slot10 now consistently holds the cavity/dual-lobe scalar weights. Slot10.x = `cavitySpecOcclusion`, the rest reserved for steps 10–11.

**Changes shipped:**

- `ext/Vulkan-Engine/include/engine/core/materials/physically_based.h` — added `DETAIL_CAVITY = 11` slot, `m_hasDetailCavityTexture` flag, `m_cavitySpecOcclusion` (default `1.0`) param with getter/setter.
- `ext/Vulkan-Engine/src/core/materials/physically_based.cpp` — slot9/slot10 packing restructured.
- `ext/Vulkan-Engine/src/core/passes/forward_pass.cpp` — `OBJECT_TEXTURE_LAYOUT` 11 → 12 bindings.
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — `detailCavityTex` sampler at binding 11; uniform block updated; per-light spec/diff split hoisted; cavity occlusion in the per-light loop.
- `src/scene_loader.cpp` — parses `detail_cavity_texture` (linear UNORM), `cavity_spec_occlusion`.
- `resources/scenes/maria.json` — wired cavity at the same tiling as detail normal.

**Verified.** User confirmed pores no longer read as shiny plastic.

---

### Detail (pore) normal blended into forward shader (Layer B step 8) (2026-06-02)

First Layer B step. Adds pore-scale microdetail to skin by sampling a high-frequency tileable normal map at scaled UVs and blending with the existing base normal in tangent space. Pores read clearly on close-ups, vanish at distance, and don't introduce specular sparkle artifacts. Whiteout blend (xy of base + xy of detail scaled by strength, z multiplied) is used over RNM/UDN for robustness at glancing angles.

**Approach.** Refactored `setupBRDFProperties` to decode the base normal into tangent space first, blend the detail normal in tangent space, then transform the combined normal to world space via `v_TBN`. Identity tangent-space normal `(0,0,1)` is used when no base normal map is bound, so the path works uniformly with or without a base normal. `MaterialUniforms` grew from 8 to 10 Vec4 slots (`uniforms.h`) to make room for Layer B parameters — extra trailing bytes are ignored by shaders that don't declare them, so other materials are unaffected.

**Changes shipped:**

- `ext/Vulkan-Engine/include/engine/graphics/uniforms.h` — added `dataSlot9` and `dataSlot10` to `MaterialUniforms`.
- `ext/Vulkan-Engine/include/engine/core/materials/physically_based.h` — added `DETAIL_NORMAL = 10` slot, `m_hasDetailNormalTexture` flag, `m_detailTiling` (default `8.0`), `m_detailNormalStrength` (default `0.5`), getter/setter pairs.
- `ext/Vulkan-Engine/src/core/materials/physically_based.cpp` — `dataSlot9 = (detailTiling, detailNormalStrength, hasDetailNormal, 0)`; `dataSlot10` zeroed (reserved for steps 9-10).
- `ext/Vulkan-Engine/src/core/passes/forward_pass.cpp` — `OBJECT_TEXTURE_LAYOUT` grew 10 → 11 bindings (added `textureBinding11` at binding 10).
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — declared `detailNormalTex` sampler at binding 10; extended `MaterialUniforms` block with `detailTiling`, `detailNormalStrength`, `hasDetailNormalTexture`, padding for slot9, and a reserved `vec4 _slot10`. Replaced the single-line `brdf.normal = ...` with tangent-space decode → optional detail blend → world-space transform.
- `src/scene_loader.cpp` — parses `detail_normal_texture` (linear UNORM), `detail_tiling`, `detail_normal_strength`; added to `warn_unknown` allowlist.
- `resources/scenes/maria.json` — wired `normal_detail.png` with `detail_tiling: 32`, `detail_normal_strength: 1.0` (user-tuned from the 16/0.4 defaults I shipped). Also dropped `roughness_weight` from `0.85` to `0.75` — likely a complementary tweak now that pore micro-occlusion would otherwise read as plastic against the smoother base.

**Verified.** User confirmed close-up pore detail visible, mid-shot subtle, and no artifacts. Tuning landed at higher tiling (denser pores) and full strength.

---

### Clothes mask gates skin-specific effects (2026-06-02)

Layer A step 6 of the Skin realism initiative. Maria's `pbr` material covers both her face and her tank top — a single shader path serves both. The clothes mask now suppresses every skin-specific effect (pre-integrated diffuse, bent-normal IBL, screen-space SSS) on clothing regions so they render as ordinary non-skin PBR, with smooth lerps across the boundary so no hard seam appears at the neckline.

**Approach.** One sample, three gates. The clothes mask is sampled once at the top of `main()` and converted to a `skinMask` scalar (`1.0 = full skin`, `0.0 = clothes`). That scalar is then used to lerp each skin effect's contribution: pre-integrated wrap delta scales by `skinMask`, bent-normal ambient lerps to plain `computeAmbient`, and the SSS pass's `outAlbedoMask.a` is multiplied by `skinMask` so the screen-space scatter is suppressed too. Convention: white = clothes, black = skin (matches the authoring of `T-Maria_ClothesMask.png`).

**Changes shipped (all `physically_based.glsl`):**

- Added `float skinMask = hasClothesMaskTexture ? (1.0 - texture(clothesMaskTex, v_uv).r) : 1.0;` at the top of `main()`, sampled once and reused.
- Pre-integrated diffuse path (step 3) now gated: `(diffWrapped - diffBase) * skinMask` for the direct contribution, `mix(lambertianIrr, wrappedIrr, skinMask)` for the SSS-feeding `diffIrrPerLight`.
- Bent-normal IBL path (step 4) lerps to plain `computeAmbient` by `skinMask`.
- `outAlbedoMask.a` is now `scatterMask * skinMask`, so SSS sees zero on clothes pixels regardless of the scattering map content.

No C++ changes; the `hasClothesMaskTexture` bit was already in `materialFlags` (slot8.y bit 2) from step 5.

**Verified.** User confirmed clothes regions are no longer carrying skin-specific effects and skin regions look identical to before. The smooth lerp at the neckline avoids a hard seam.

---

### Scattering map → per-texel SSS modulation (2026-06-02)

Layer A step 5 of the Skin realism initiative. The screen-space SSS pass previously applied a globally-uniform scatter radius to every "skin" pixel. The scattering map (`T-Maria-Scattering.png`) authored per character now drives a per-pixel modulation so thin/translucent regions (ears, lips, nose tip, eyelids) scatter strongly while thick regions (forehead, mid-cheek, jaw) stay closer to local diffuse.

**Approach.** `outAlbedoMask.a` already existed as a binary skin-vs-non-skin gate in the SSS pass (line 86 of `ssss.glsl`). Generalized that channel to carry a continuous `[0, 1]` per-pixel mask. The SSS pass now lerps between local diffuse `(albedo / PI) * diffIrr` and the blurred `scatteredIrr`, and scales single-scatter, by the per-pixel mask. At mask = 1 the result matches the previous behaviour exactly; below that, the local diffuse and zero single-scatter take over proportionally.

**Slot8 packing decision.** Two more flags needed slots; `slot8` was full. Solution: repurpose `slot8.y` (previously `m_isReflective`, declared but unused in any shader) as a bit-packed `materialFlags` float — bit 0 = `isReflective`, bit 1 = `hasScatteringTexture`, bit 2 = `hasClothesMaskTexture`. Shader extracts via `int flags = int(material.materialFlags); bool hasX = (flags & N) != 0;`. Slots `.z` and `.w` retain curvature and bent-normal flags as plain `bool`s. This pattern can be reused for any future single-bit-per-material flags without growing the uniform.

**Changes shipped:**

- `ext/Vulkan-Engine/src/core/materials/physically_based.cpp` — slot8.y rewritten as the packed `materialFlags` int (cast to float for transport).
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — `MaterialUniforms` (VS + FS) replaces `bool isReflective` with `float materialFlags`. At MRT write, samples `scatteringTex` when the flag is set and writes the value into `outAlbedoMask.a`; without the texture, writes `1.0` (preserves previous full-SSS behaviour).
- `ext/Vulkan-Engine/resources/shaders/misc/ssss.glsl` — final combine now lerps `modulatedDiff = mix(localDiff, scatteredIrr, scatterMask)`, scales `modulatedSS = singleScatter * scatterMask`, and extracts specular by subtracting the modulated diffuse so total energy stays consistent at every mask value.

**Verified.** User confirmed ears/lips/nose-tip read more translucent than before and forehead/jaw closer to base PBR diffuse. Anticipated step 6 behaviour also observed (clothes regions where the scattering map was painted 0 already got skipped from SSS, then made fully consistent in step 6 via the clothes mask).

---

### Bent normal → diffuse IBL direction (2026-06-02)

Layer A step 4 of the Skin realism initiative. When a bent normal map is bound on a `pbr` material, the IBL diffuse ambient lookup samples the irradiance cube along the bent normal (the average unoccluded direction) instead of the geometric normal. Crevices and underhangs now read incoming light from the direction it actually reaches them. Fresnel/specular keep using the geometric normal — that's the correct microfacet response — so this is a diffuse-only redirection.

**Approach.** Added a sibling function in `IBL.glsl` (`computeAmbientBentNormal`) rather than modifying the shared `computeAmbient` signature — three other shaders call it (`hair_card`, deferred `composition`, and the hair card variants), and growing the signature for one use case isn't worth the churn. The shader picks which variant to call based on `material.hasBentNormalTexture`.

**Changes shipped:**

- `ext/Vulkan-Engine/src/core/materials/physically_based.cpp` — packed `m_hasBentNormalTexture` into `dataSlot8.w`. With this, all four Layer A flags from step 2 (curvature, bent normal, scattering, clothes mask) have a uniform path — slot8 currently carries curvature in `.z` and bent normal in `.w`; scattering and clothes mask will need a new packing scheme when their steps land (slot8 is full).
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — added `bool hasBentNormalTexture;` to the `MaterialUniforms` block (both VS and FS copies). In the IBL ambient branch, when the flag is set, decode `bentNormalTex` as tangent-space (same convention as the regular normal map), transform via `v_TBN` to world space, and call the new variant.
- `ext/Vulkan-Engine/resources/shaders/scripts/IBL.glsl` — added `computeAmbientBentNormal(samplerCube, envRotation, worldNormal, bentNormal, camPos, albedo, F0, metalness, roughness, intensity)`. Identical to `computeAmbient` except the irradiance cube is sampled along the rotated bent normal while Fresnel uses the rotated geometric normal.

**Verified.** User confirmed the render is visually similar to the previous step with no broken regions — the expected outcome for Maria's near-uniform studio HDRi, where bent-normal redirection gives subtle results. The effect earns more visual weight in scenes with strong directional environment light (outdoor HDRi with bright sky vs dark ground), so the absence of dramatic change here is not a sign the path is wrong.

**Slot8 packing note (carrying forward).** All four flag fields are now used:
- `.x` = emissionIntensity
- `.y` = isReflective
- `.z` = hasCurvatureTexture
- `.w` = hasBentNormalTexture

Steps 5 (scattering) and 6 (clothes mask) need two more flags. Options: bit-pack into one existing float, repurpose `m_maskType` (currently `int` 0–2 with most bits unused), or extend `MaterialUniforms` to a 9th Vec4. Decision deferred to step 5.

---

### Curvature → pre-integrated skin diffuse (Penner 2011, analytical) (2026-06-02)

Layer A step 3 of the Skin realism initiative. Replaces the per-light Lambertian `NdotL` term with a curvature-aware, per-channel wrapped response for skin materials, so the shadow terminator on curved features (nose bridge, cheekbones, jaw) gets the characteristic warm red wraparound. Composes cleanly with the existing screen-space SSS — pre-integrated handles local curvature, SSS handles broader inter-pixel scatter; the chromatic wrapped value is fed into the `diffuseIrr` MRT so SSS blurs it too.

**Approach.** Chose the analytical Brisebois-Hoffman variant over a baked 2D LUT: ~20 lines of GLSL, no new resources, no engine plumbing, and visually ~95% of the baked version. The chromatic shift falls out naturally from per-channel wrap widths matching skin's relative scatter distances (R=1.0, G=0.4, B=0.2 — Burley defaults) — no explicit tint table.

**Changes shipped:**

- `ext/Vulkan-Engine/src/core/materials/physically_based.cpp` — packed `m_hasCurvatureTexture` into `dataSlot8.z`. Also rewrote slot8 to explicitly cast `m_isReflective` to `1.0f/0.0f` (was an implicit bool→float that other slots already did).
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — extended `MaterialUniforms` (both VS and FS blocks) with `bool isReflective; bool hasCurvatureTexture;` after `emissionIntensity`. Added `preIntegratedSkinDiffuse(NdotL, curvature)` helper. In the per-light loop, when `hasCurvatureTexture` is true, locally recompute `kD` from Fresnel, subtract the Lambertian diffuse contribution embedded in the BRDF call, and add the wrapped contribution. Same wrapped value (without albedo/π prefactor) is written into `diffuseIrr` so the SSS pass blurs it.

**Tuning landed.** Wrap scale constant in `w = 0.35 * curvature * channelWrap`. Initial value `0.5` produced visibly correct wraparound but read too warm on Maria's chin/neck under SSS-on; `0.35` was the natural-looking setting. The constant is the per-skin-author calibration knob for how strongly the curvature map drives wraparound.

**Verified.** User compared SSS-off (clear orange band at terminators on curved features, none on flat forehead — confirms texture is `0=flat, 1=curved` convention, not center-flat) vs SSS-on (warm soft wraparound on cheek/nose/chin, natural). After dropping wrap scale to 0.35 with SSS on, render reads as natural fleshy skin.

**Known caveat for later steps.** Maria's `pbr` material currently covers both skin and clothes — the clothes mask isn't wired into the shader yet (step 6 of the initiative), so any curvature painted into clothing regions will also get the wrap. Effect is mild because clothes typically have low painted curvature, but step 6 will gate this off explicitly.

---

### PBR material extension for BentNormal / Curvature / Scattering / ClothesMask (2026-06-02)

Layer A of the Skin realism initiative: extend `PhysicallyBasedMaterial` with the 4 authored map types shipped under `resources/textures/<character>/` that had no engine slot. Samplers are declared but no shader logic yet — that lands per-map in subsequent steps (curvature → pre-integrated diffuse, bent normal → IBL diffuse direction, scattering → per-texel SSS modulation, clothes mask → SSS gate).

**Changes shipped:**

- `ext/Vulkan-Engine/include/engine/core/materials/physically_based.h` — added `BENT_NORMAL=6`, `CURVATURE=7`, `SCATTERING=8`, `CLOTHES_MASK=9` to the `Textures` enum; added `m_hasBentNormalTexture` / `m_hasCurvatureTexture` / `m_hasScatteringTexture` / `m_hasClothesMaskTexture` flags; added matching `get_*_texture` / `set_*_texture` pairs; expanded the `m_textures` map.
- `ext/Vulkan-Engine/src/core/passes/forward_pass.cpp` — grew `OBJECT_TEXTURE_LAYOUT` from 7 to 10 fragment-stage combined-image-sampler bindings (added `textureBinding8/9/10` at bindings 7/8/9).
- `ext/Vulkan-Engine/resources/shaders/forward/physically_based.glsl` — declared `bentNormalTex`, `curvatureTex`, `scatteringTex`, `clothesMaskTex` samplers at set=2 bindings 6–9. No fragment-shader logic touches them yet.
- `src/scene_loader.cpp` — parses `bent_normal_texture` (linear UNORM, like the main normal map), `curvature_texture` / `scattering_texture` / `clothes_mask_texture` (linear data maps via `TEXTURE_FORMAT_TYPE_LINEAR`); added all four to the `warn_unknown` allowlist.
- `resources/scenes/maria.json` — wired all 4 new maps so the descriptor writes exercise the path.

`geometry_pass` and the deferred path were intentionally skipped: HairViewer uses the forward renderer, and growing the deferred layout adds risk for no current benefit. Uniform `has*Texture` flags weren't packed into `MaterialUniforms` yet — they'll be added one slot at a time as each map gets shader logic, since `MaterialUniforms` has limited free room in `dataSlot8` and packing everything up front would be premature.

**Verified.** User built and ran HairViewer; render is visually identical to the previous step (correct — samplers declared but unread) and no new debug-layer warnings appeared.

---

### Maria authored skin maps wiring + data-map sRGB-vs-linear fix (2026-06-02)

Maria's material was using the GLB-embedded base color (`$GLB[0]`) with no normal/roughness/AO map — skin read as a flat tone. The repo already shipped a full authored map set under `resources/textures/maria/` (`T-Maria_BaseColor.png`, `T-Maria-Normal.png`, `T-Maria-Roughness.png`, `T-Maria-AO.png`, plus four more — BentNormal, Curvature, Scattering, ClothesMask — for which no engine slot exists yet; tracked as step 2 of the Skin realism initiative). Wiring the first four into `resources/scenes/maria.json` surfaced a second bug:

**Issue found.** `src/scene_loader.cpp` was resolving `roughness_texture`, `metallic_texture` and `occlusion_texture` with `TEXTURE_FORMAT_TYPE_COLOR`, which selects `SRGBA_8` in `loaders.cpp::load_PNG`. These are data maps and must sample linearly. The sRGB→linear curve was darkening every value (painted 0.5 → ~0.21 in shader), making Maria's skin look wet and plastic at `roughness_weight = 1.0`.

**Fixes shipped:**

- `ext/Vulkan-Engine/include/engine/common.h` — added `TEXTURE_FORMAT_TYPE_LINEAR` as an alias for `TEXTURE_FORMAT_TYPE_NORMAL` (which was always linear UNORM — the comment at `sss_pass.cpp:218` already confirmed this). Alias is for readability so future readers don't think a roughness map is being treated as a normal map.
- `src/scene_loader.cpp` — `roughness_texture`, `metallic_texture`, `occlusion_texture` now resolved with `TEXTURE_FORMAT_TYPE_LINEAR`. Albedo and emissive stay on `TEXTURE_FORMAT_TYPE_COLOR` (genuinely sRGB).
- `resources/scenes/maria.json` — material now reads the 4 authored maps explicitly (no longer `$GLB[0]`). Tuned `roughness_weight: 0.85` (small bias toward matte over the painted texture).

**Verified.** User confirmed end-to-end visual result on Maria: no more wet-plastic skin on shoulders, subtle remaining specular on cheekbones at `roughness_weight: 0.85`. Wiring + sRGB fix together unlocked using `roughness_weight: 1.0`–level fidelity without the original gloss problem (the 0.85 is a small artistic damp, not a workaround).

---

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
