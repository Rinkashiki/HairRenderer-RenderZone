#!/usr/bin/env python3
"""
bake_glb_material.py — bake scene-JSON materials + textures into a self-contained GLB.

Reads a scene JSON (see SCENE.md), finds its `glb` character mesh, and for every
glTF material embeds the engine material block the scene currently defines:

  * Standard glTF slots (baseColor / metallic-roughness / occlusion / normal) are
    written so the GLB opens sensibly in any glTF viewer. Roughness + Metallic + AO
    live in a single ORM image (R=occlusion, G=roughness, B=metallic) as glTF requires —
    either supplied pre-packed by the artist, or repacked here from separate maps.
  * The full engine material block is written verbatim into
    `material.extras.vkfw_material` (a JSON string), with every `*_texture` path
    replaced by a `$GLB[<image name>]` reference (or `$GLB[<image>:<channel>]` for a
    channel of a packed atlas). The engine reads THIS block; the standard slots are
    decoration for external tools.

Packed source maps
------------------
A texture path may carry a channel suffix, which means "this map is one channel of a
shared atlas". Embed the atlas once, reference it per slot:

    "occlusion_texture":  "textures/alex/T-Alex-ORM.png:r",
    "roughness_texture":  "textures/alex/T-Alex-ORM.png:g",
    "metallic_texture":   "textures/alex/T-Alex-ORM.png:b",
    "curvature_texture":  "textures/alex/T-Alex-CS.png:r",
    "scattering_texture": "textures/alex/T-Alex-CS.png:g"

The engine binds ONE decoded texture for all of them and samples the right channel per
slot, so a packed atlas costs one image on disk and one texture in VRAM. Slots without a
suffix behave as before (whole image, sampled from R). Separate occlusion/roughness/
metallic maps with no suffix are still auto-repacked into an ORM here.

Material source
---------------
By default the blocks come from the scene JSON's `glb` mesh entry. Once a scene has been
slimmed (its materials now live in the GLB) there is nothing left to read, so pass a
recipe instead — a small JSON holding just the material fields:

    { "material": {...}, "extra_materials": [...], "primitive_materials": [...] }

Recipes live in `tools/bake_recipes/<character>.json` and are the re-bakeable source of
truth for each character's materials.

Then it emits a slimmed scene JSON with the baked `material` / `extra_materials` /
`primitive_materials` fields removed from the character mesh (they now live in the GLB).
The loader merges any material fields the scene still declares OVER the baked block, so
a partially-baked GLB + JSON that fills the gaps still works.

Usage:
    python tools/bake_glb_material.py resources/scenes/nadia.json
    python tools/bake_glb_material.py resources/scenes/nadia.json \
        --materials tools/bake_recipes/nadia.json --in-glb nadia.orig.glb --inplace
    python tools/bake_glb_material.py resources/scenes/nadia.json --suffix .baked   # safe dev output

Defaults (no --out-*): writes <glb>.baked.glb and <scene>.baked.json so originals
are never clobbered. Pass --inplace to overwrite both.
"""
import argparse
import copy
import json
import os
import sys
from io import BytesIO
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("This tool needs Pillow: pip install Pillow")
try:
    import pygltflib
    from pygltflib import (GLTF2, BufferView, Image as GLTFImage, Sampler,
                           Texture as GLTFTexture, TextureInfo, PbrMetallicRoughness)
except ImportError:
    sys.exit("This tool needs pygltflib: pip install pygltflib")

# Engine material texture-slot keys (mirror scene_loader.cpp build_pbr).
TEXTURE_KEYS = [
    "albedo_texture", "normal_texture", "roughness_texture", "metallic_texture",
    "occlusion_texture", "emissive_texture", "bent_normal_texture",
    "curvature_texture", "scattering_texture", "clothes_mask_texture",
    "eye_mask_texture", "detail_normal_texture", "detail_cavity_texture",
]
# Slots repacked into one ORM image (channel index in RGBA) when supplied unpacked.
ORM_KEYS = {"occlusion_texture": 0, "roughness_texture": 1, "metallic_texture": 2}
CHANNELS = "rgba"


def log(msg):
    print("[bake] " + msg)


def image_name(path):
    """Stable, unique-per-character image name from a texture path."""
    return Path(path).stem


def split_ref(val):
    """'textures/x/ORM.png:g' -> ('textures/x/ORM.png', 1). No suffix -> (path, None)."""
    if not isinstance(val, str) or not val:
        return None, None
    head, sep, tail = val.rpartition(":")
    if sep and len(tail) == 1 and tail in CHANNELS:
        return head, CHANNELS.index(tail)
    return val, None


# ─── binary-blob surgery ─────────────────────────────────────────────────────

def align4(blob):
    while len(blob) % 4 != 0:
        blob.append(0)


def add_png(gltf, blob, png_bytes, name, img_cache):
    """Append a PNG to buffer 0, add image+texture, return texture index (deduped by name)."""
    if name in img_cache:
        return img_cache[name]
    align4(blob)
    offset = len(blob)
    blob.extend(png_bytes)
    gltf.bufferViews.append(BufferView(buffer=0, byteOffset=offset, byteLength=len(png_bytes)))
    bv_idx = len(gltf.bufferViews) - 1
    gltf.images.append(GLTFImage(mimeType="image/png", bufferView=bv_idx, name=name))
    img_idx = len(gltf.images) - 1
    sampler = 0 if gltf.samplers else None
    gltf.textures.append(GLTFTexture(source=img_idx, sampler=sampler))
    tex_idx = len(gltf.textures) - 1
    img_cache[name] = tex_idx
    log(f"  embedded image '{name}' ({len(png_bytes)//1024} KB) -> texture[{tex_idx}]")
    return tex_idx


def png_bytes_of(img):
    buf = BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def load_gray(path):
    """Load a texture as a single-channel (L) image."""
    return Image.open(path).convert("L")


# ─── material baking ─────────────────────────────────────────────────────────

def build_orm(block, resources):
    """Repack separate occlusion/roughness/metallic grayscale maps into one RGBA ORM
    image. Slots already pointing at a packed atlas (`path:channel`) are skipped — they
    are embedded as-is. Returns (PIL.Image, name) or (None, None) if there is nothing
    left to pack."""
    present = {k: block[k] for k in ORM_KEYS
               if isinstance(block.get(k), str) and block[k] and split_ref(block[k])[1] is None}
    if not present:
        return None, None
    # Base size from the first present channel.
    first_path = resources / next(iter(present.values()))
    size = Image.open(first_path).size
    channels = [Image.new("L", size, 0), Image.new("L", size, 0), Image.new("L", size, 0)]
    for key, ci in ORM_KEYS.items():
        p = present.get(key)
        if p:
            g = load_gray(resources / p)
            if g.size != size:
                g = g.resize(size)
            channels[ci] = g
    orm = Image.merge("RGB", channels)  # R=occlusion, G=roughness, B=metallic (alpha unused)
    name = "orm_" + "_".join(sorted(image_name(v) for v in present.values()))
    return orm, name


def bake_material(gltf, blob, block, resources, img_cache):
    """Embed a slot's engine material block. Returns (extras_block, std_slots) where
    extras_block is the engine JSON with $GLB refs and std_slots describes glTF slots."""
    extras = copy.deepcopy(block)
    std = {"baseColor": None, "normal": None, "orm": None,
           "baseColorFactor": None, "metallicFactor": None, "roughnessFactor": None}

    # Legacy path: separate occlusion/roughness/metallic grayscale maps -> one ORM.
    orm_img, orm_name = build_orm(block, resources)
    if orm_img is not None:
        orm_tex = add_png(gltf, blob, png_bytes_of(orm_img), orm_name, img_cache)
        std["orm"] = orm_tex
        for key, ci in ORM_KEYS.items():
            if isinstance(block.get(key), str) and block[key] and split_ref(block[key])[1] is None:
                extras[key] = f"$GLB[{orm_name}:{CHANNELS[ci]}]"

    # Every remaining texture -> embed the original file bytes verbatim (identical
    # pixels, native compression; the engine forces RGBA at decode time regardless).
    # A `path:channel` source is an atlas: embedded once (add_png dedupes by name),
    # referenced per slot with its channel.
    for key in TEXTURE_KEYS:
        val = block.get(key)
        if not isinstance(val, str) or not val:
            continue
        path, channel = split_ref(val)
        if channel is None and key in ORM_KEYS:
            continue  # already folded into the built ORM above
        name = image_name(path)
        tex = add_png(gltf, blob, (resources / path).read_bytes(), name, img_cache)
        extras[key] = f"$GLB[{name}]" if channel is None else f"$GLB[{name}:{CHANNELS[channel]}]"
        if key == "albedo_texture":
            std["baseColor"] = tex
        elif key == "normal_texture":
            std["normal"] = tex
        elif key in ORM_KEYS:
            # Pre-packed ORM supplied by the artist — use it for the glTF slots directly.
            std["orm"] = tex

    # Standard glTF scalar factors (best-effort, decoration only).
    if isinstance(block.get("albedo"), list) and len(block["albedo"]) >= 3:
        a = block["albedo"]
        std["baseColorFactor"] = [a[0], a[1], a[2], block.get("opacity", 1.0)]
    if "metalness" in block:
        std["metallicFactor"] = float(block["metalness"])
    if "roughness" in block and not isinstance(block.get("roughness"), str):
        std["roughnessFactor"] = float(block["roughness"])

    return extras, std


def apply_std_slots(gmat, std):
    """Write standard glTF slots onto a pygltflib Material (for external viewers)."""
    pbr = gmat.pbrMetallicRoughness or PbrMetallicRoughness()
    if std["baseColor"] is not None:
        pbr.baseColorTexture = TextureInfo(index=std["baseColor"])
    if std["orm"] is not None:
        pbr.metallicRoughnessTexture = TextureInfo(index=std["orm"])
        from pygltflib import OcclusionTextureInfo
        gmat.occlusionTexture = OcclusionTextureInfo(index=std["orm"])
    if std["baseColorFactor"] is not None:
        pbr.baseColorFactor = std["baseColorFactor"]
    if std["metallicFactor"] is not None:
        pbr.metallicFactor = std["metallicFactor"]
    if std["roughnessFactor"] is not None:
        pbr.roughnessFactor = std["roughnessFactor"]
    gmat.pbrMetallicRoughness = pbr
    if std["normal"] is not None:
        from pygltflib import NormalMaterialTexture
        gmat.normalTexture = NormalMaterialTexture(index=std["normal"])


# ─── slot resolution ─────────────────────────────────────────────────────────

def resolve_blocks(mesh_entry):
    """Return {slot_index: engine_material_block} from the scene mesh entry.
    Slot 0 = 'material', slot i+1 = extra_materials[i]. Only inline dict blocks
    are bakeable (library references/`base` overrides are skipped with a warning)."""
    slots = {}
    m = mesh_entry.get("material")
    if isinstance(m, dict) and "base" not in m:
        slots[0] = m
    elif m is not None:
        log("  WARN: primary material is a library ref / base-override; not baking slot 0")
    for i, em in enumerate(mesh_entry.get("extra_materials", []) or []):
        if isinstance(em, dict) and "base" not in em:
            slots[i + 1] = em
        else:
            log(f"  WARN: extra_materials[{i}] is a library ref / base-override; not baking")
    return slots


def geom_material_indices(gltf):
    """glTF material index per engine geometry (meshes in order, one entry per primitive)."""
    out = []
    for m in gltf.meshes:
        for p in m.primitives:
            out.append(p.material if p.material is not None else -1)
    return out


def main():
    ap = argparse.ArgumentParser(description="Bake scene-JSON materials into a self-contained GLB.")
    ap.add_argument("scene", help="scene JSON path")
    ap.add_argument("--resources", help="resources root (default: <scene>/../..)")
    ap.add_argument("--materials", help="recipe JSON holding material/extra_materials/"
                                        "primitive_materials (use once the scene is slimmed)")
    ap.add_argument("--in-glb", help="source GLB to bake (default: the scene's glb mesh file). "
                                     "Point this at the unbaked original when re-baking.")
    ap.add_argument("--out-glb", help="output GLB path")
    ap.add_argument("--out-scene", help="output slimmed scene JSON path")
    ap.add_argument("--suffix", default=".baked", help="suffix for default outputs (default: .baked)")
    ap.add_argument("--inplace", action="store_true", help="overwrite the original GLB and scene JSON")
    ap.add_argument("--force", action="store_true", help="bake even if the input GLB is already baked")
    args = ap.parse_args()

    scene_path = Path(args.scene).resolve()
    scene = json.loads(scene_path.read_text())
    resources = Path(args.resources).resolve() if args.resources else scene_path.parent.parent

    # Locate the character glb mesh.
    meshes = scene.get("meshes", [])
    glb_idx = next((i for i, m in enumerate(meshes) if m.get("type") == "glb"), None)
    if glb_idx is None:
        sys.exit("no glb mesh in scene")
    mesh_entry = meshes[glb_idx]
    glb_path = resources / mesh_entry["file"]
    in_glb = Path(args.in_glb).resolve() if args.in_glb else glb_path

    # Material source: a recipe if given (the scene may already be slimmed), else the
    # scene's own mesh entry.
    if args.materials:
        src = json.loads(Path(args.materials).read_text())
        log(f"materials from recipe {args.materials}")
    else:
        src = mesh_entry
    log(f"scene={scene_path.name}  in={in_glb.name}  resources={resources}")

    slots = resolve_blocks(src)
    if not slots:
        sys.exit("no bakeable inline materials found (pass --materials <recipe.json> "
                 "if the scene has already been slimmed)")
    prim_mats = src.get("primitive_materials")

    gltf = GLTF2().load(str(in_glb))

    # Baking appends images; re-baking an already-baked GLB would keep the old ones as
    # orphaned buffer data. Re-bake from the unbaked original instead.
    already = [i for i, m in enumerate(gltf.materials)
               if isinstance(m.extras, dict) and "vkfw_material" in m.extras]
    if already and not args.force:
        sys.exit(f"{in_glb.name} is already baked (materials {already}). Pass --in-glb "
                 f"<unbaked original>, or --force to bake on top anyway.")

    blob = bytearray(gltf.binary_blob())
    if not gltf.samplers:
        gltf.samplers.append(Sampler())

    # Map glTF material index -> slot (via primitive_materials, else identity by geom order).
    gmat_of_geom = geom_material_indices(gltf)
    n_geom = len(gmat_of_geom)
    slot_of_geom = prim_mats if prim_mats else list(range(n_geom))
    gmat_to_slot = {}
    for g in range(n_geom):
        gm = gmat_of_geom[g]
        slot = slot_of_geom[g] if g < len(slot_of_geom) else g
        if gm < 0:
            continue
        if gm in gmat_to_slot and gmat_to_slot[gm] != slot:
            log(f"  WARN: glTF material {gm} maps to slots {gmat_to_slot[gm]} and {slot}; using first")
        else:
            gmat_to_slot.setdefault(gm, slot)

    img_cache = {}
    baked = 0
    for gm_idx, gmat in enumerate(gltf.materials):
        slot = gmat_to_slot.get(gm_idx)
        if slot is None or slot not in slots:
            log(f"  material[{gm_idx}] '{gmat.name}' -> no bakeable slot; left as-is")
            continue
        log(f"  material[{gm_idx}] '{gmat.name}' <- slot {slot}")
        extras, std = bake_material(gltf, blob, slots[slot], resources, img_cache)
        apply_std_slots(gmat, std)
        ex = gmat.extras if isinstance(gmat.extras, dict) else {}
        ex["vkfw_material"] = json.dumps(extras, separators=(",", ":"))
        gmat.extras = ex
        baked += 1

    align4(blob)
    gltf.set_binary_blob(bytes(blob))
    gltf.buffers[0].byteLength = len(blob)

    # Outputs.
    if args.inplace:
        out_glb = glb_path
        out_scene = scene_path
    else:
        out_glb = Path(args.out_glb) if args.out_glb else glb_path.with_suffix(args.suffix + ".glb")
        out_scene = Path(args.out_scene) if args.out_scene else scene_path.with_suffix(args.suffix + ".json")

    gltf.save(str(out_glb))
    log(f"wrote {out_glb}  ({os.path.getsize(out_glb)//1024} KB, {baked} materials baked, "
        f"{len(img_cache)} images)")

    # Slim the scene: drop baked material fields from the glb mesh entry.
    slim = copy.deepcopy(scene)
    sm = slim["meshes"][glb_idx]
    removed = [k for k in ("material", "extra_materials", "primitive_materials") if k in sm]
    for k in removed:
        sm.pop(k)
    if not removed and args.inplace:
        log(f"scene {scene_path.name} already slim; left untouched")
        return
    # For non-inplace output, repoint the mesh at the baked GLB so the slimmed scene
    # is self-consistent (originals stay untouched for A/B comparison).
    if not args.inplace:
        sm["file"] = str(out_glb.relative_to(resources)).replace(os.sep, "/")
    out_scene.write_text(json.dumps(slim, indent=2))
    log(f"wrote {out_scene}  (slimmed, glb -> {sm['file']})")


if __name__ == "__main__":
    main()
