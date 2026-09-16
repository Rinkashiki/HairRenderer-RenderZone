#!/usr/bin/env python3
"""
rig_teeth_glb.py — prepare a Blender re-export of a character + the new teeth model
for this engine, and emit the bake recipe for it.

Workflow per character:
    1. Blender: open <char>.glb, import the teeth model, place it, export (defaults).
    2. python tools/rig_teeth_glb.py <export.glb> --out <rigged.glb> \
           --recipe tools/bake_recipes/<char>.json
    3. python tools/bake_glb_material.py resources/scenes/<char>.json \
           --materials tools/bake_recipes/<char>.json --in-glb <rigged.glb> --inplace
    4. HairViewer: re-seat + Bind + Save every strand-hair groom (the head's
       topology changed, so the old .hbnd sidecars no longer apply).

What this script does to the export (the engine's GLB loader needs all of it):
  * Drops helper meshes that are invisible by design (default: GumsAttachmentOverlay_*).
  * Bakes each mouth mesh's node transform (relative to the face node) into its
    vertices — the loader reads gltf.meshes directly and ignores node transforms.
  * Gives every mouth mesh 100%-weight skinning to a single joint using the face's
    skin: "*Lower*" + Tongue -> jaw, "*Upper*" + Uvula -> head. The raw export has
    no skinning on them at all, so they'd never follow the jaw.
  * Puts the face mesh first (geometry[0]) — several engine paths (animation
    sampling, mesh bounds, hair binding) take geometry[0] as "the head".
  * Detects cavity-style meshes (normals pointing inward, meant to be seen from
    inside only) and gives them their own glTF material so the bake can put a
    front-culling material on them. Materials end up ordered 0=face, 1=teeth,
    2=cavity, matching the recipe's slots.
  * Keeps any already-skinned accessory meshes (e.g. Javi's hearing aid) as they
    are, after the mouth meshes, with their own material slots; their engine
    material blocks are copied from the previous bake by name (--old-glb).
  * Strips the images Blender re-embedded (the bake embeds the real ones from
    resources/textures) and compacts the buffer so nothing orphaned is left.

Requires pygltflib and numpy.
"""
import argparse
import copy
import json
import struct
import sys
from pathlib import Path

import numpy as np
import pygltflib
from pygltflib import GLTF2, Accessor, BufferView, Material

DEFAULT_DROP = ("GumsAttachmentOverlay",)

TEETH_TEXTURES = {
    "albedo_texture":    "textures/teeth/T_Teeth_BaseColor.png",
    "normal_texture":    "textures/teeth/T_Teeth_Normal.png",
    "occlusion_texture": "textures/teeth/T_Teeth_ORM.png:r",
    "roughness_texture": "textures/teeth/T_Teeth_ORM.png:g",
    "metallic_texture":  "textures/teeth/T_Teeth_ORM.png:b",
}


def log(msg):
    print("[rig] " + msg)


def teeth_material_block():
    return {"type": "pbr", **TEETH_TEXTURES,
            "albedo": [1.0, 1.0, 1.0], "albedo_weight": 1.0,
            "roughness_weight": 1.0, "occlusion_weight": 1.0, "metalness": 0.0}


def cavity_material_block():
    b = teeth_material_block()
    b["culling"] = "front"  # inward-facing shell: show the inside only
    return b


# ─── glTF helpers ───────────────────────────────────────────────────────────

def align4(blob):
    while len(blob) % 4 != 0:
        blob.append(0)


def node_local_matrix(n):
    if n.matrix:
        return np.array(n.matrix, dtype=np.float64).reshape(4, 4).T  # column-major -> row-major
    t = np.array(n.translation or [0, 0, 0], dtype=np.float64)
    q = np.array(n.rotation or [0, 0, 0, 1], dtype=np.float64)  # x y z w
    s = np.array(n.scale or [1, 1, 1], dtype=np.float64)
    x, y, z, w = q
    R = np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w)],
        [2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)],
    ])
    M = np.eye(4)
    M[:3, :3] = R * s
    M[:3, 3] = t
    return M


def node_world_matrix(gltf, idx, parent_of):
    M = node_local_matrix(gltf.nodes[idx])
    p = parent_of.get(idx)
    return node_world_matrix(gltf, p, parent_of) @ M if p is not None else M


def build_parent_map(gltf):
    parent_of = {}
    for i, n in enumerate(gltf.nodes):
        for c in (n.children or []):
            parent_of[c] = i
    return parent_of


def node_by_name(gltf, name):
    for i, n in enumerate(gltf.nodes):
        if n.name == name:
            return i
    return None


def acc_start(gltf, acc_idx):
    acc = gltf.accessors[acc_idx]
    bv = gltf.bufferViews[acc.bufferView]
    return (bv.byteOffset or 0) + (acc.byteOffset or 0), acc


def read_vec3(gltf, blob, acc_idx):
    start, acc = acc_start(gltf, acc_idx)
    assert acc.componentType == pygltflib.FLOAT and acc.type == "VEC3", "expected float VEC3"
    return np.array(struct.unpack_from(f"<{acc.count * 3}f", blob, start)).reshape(-1, 3), start


def write_vec3(blob, start, arr):
    struct.pack_into(f"<{arr.size}f", blob, start, *arr.astype(np.float32).ravel())


def read_indices(gltf, blob, acc_idx):
    start, acc = acc_start(gltf, acc_idx)
    fmt = {5121: "B", 5123: "H", 5125: "I"}[acc.componentType]
    return np.array(struct.unpack_from(f"<{acc.count}{fmt}", blob, start))


def fraction_inward(pos, idx):
    tris = idx.reshape(-1, 3)
    v0, v1, v2 = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    fn = np.cross(v1 - v0, v2 - v0)
    out = (v0 + v1 + v2) / 3.0 - pos.mean(axis=0)
    fn /= np.linalg.norm(fn, axis=1, keepdims=True) + 1e-12
    out /= np.linalg.norm(out, axis=1, keepdims=True) + 1e-12
    return float((np.sum(fn * out, axis=1) < 0).mean())


def append_accessor(gltf, blob, data_bytes, component_type, acc_type, count):
    align4(blob)
    off = len(blob)
    blob.extend(data_bytes)
    gltf.bufferViews.append(BufferView(buffer=0, byteOffset=off, byteLength=len(data_bytes)))
    gltf.accessors.append(Accessor(bufferView=len(gltf.bufferViews) - 1, componentType=component_type,
                                   count=count, type=acc_type))
    return len(gltf.accessors) - 1


def compact_buffer(gltf, blob):
    """Rebuild buffer 0 keeping only bufferViews still referenced by an accessor
    (dense, or sparse indices/values). Everything else — Blender's embedded
    images in particular — is dropped."""
    used = set()
    for a in gltf.accessors:
        if a.bufferView is not None:
            used.add(a.bufferView)
        if a.sparse is not None:
            used.add(a.sparse.indices.bufferView)
            used.add(a.sparse.values.bufferView)
    new_blob = bytearray()
    remap = {}
    new_views = []
    for old_idx in sorted(used):
        bv = gltf.bufferViews[old_idx]
        align4(new_blob)
        start = (bv.byteOffset or 0)
        chunk = blob[start:start + bv.byteLength]
        nbv = copy.deepcopy(bv)
        nbv.byteOffset = len(new_blob)
        new_blob.extend(chunk)
        remap[old_idx] = len(new_views)
        new_views.append(nbv)
    align4(new_blob)
    for a in gltf.accessors:
        if a.bufferView is not None:
            a.bufferView = remap[a.bufferView]
        if a.sparse is not None:
            a.sparse.indices.bufferView = remap[a.sparse.indices.bufferView]
            a.sparse.values.bufferView = remap[a.sparse.values.bufferView]
    gltf.bufferViews = new_views
    gltf.set_binary_blob(bytes(new_blob))
    gltf.buffers[0].byteLength = len(new_blob)
    return len(blob) - len(new_blob)


def strip_textures(mat):
    """Drop every texture reference / material extension; the bake re-adds the
    engine's own from resources/textures."""
    if mat.pbrMetallicRoughness is not None:
        mat.pbrMetallicRoughness.baseColorTexture = None
        mat.pbrMetallicRoughness.metallicRoughnessTexture = None
    mat.normalTexture = None
    mat.occlusionTexture = None
    mat.emissiveTexture = None
    mat.extensions = {}
    return mat


# ─── main ───────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Rig a Blender character+teeth export for the engine.")
    ap.add_argument("input", help="GLB exported from Blender (character + placed teeth model)")
    ap.add_argument("--out", required=True, help="rigged (still unbaked) GLB to write")
    ap.add_argument("--recipe", help="existing bake recipe (tools/bake_recipes/<char>.json); its "
                                     "'material' block is the face. Updated in place unless --out-recipe")
    ap.add_argument("--out-recipe", help="where to write the updated recipe (default: --recipe path)")
    ap.add_argument("--face-node", help="face/body node name (default: the skinned node that has morph targets)")
    ap.add_argument("--old-glb", help="previous baked <char>.glb; material blocks for accessory meshes "
                                      "(already-skinned extras like Javi's hearing aid) are copied from it by name")
    ap.add_argument("--jaw", default="jaw", help="joint driving the lower mouth (default: jaw)")
    ap.add_argument("--head", default="head", help="joint driving the upper mouth (default: head)")
    ap.add_argument("--drop", nargs="*", default=list(DEFAULT_DROP),
                    help="node-name prefixes of helper meshes to drop")
    ap.add_argument("--bind", nargs="*", default=[], metavar="NODE=jaw|head",
                    help="override / add joint assignment for a mouth node")
    ap.add_argument("--cavity-threshold", type=float, default=0.5,
                    help="fraction of inward-facing triangles above which a mesh is treated as a cavity shell")
    args = ap.parse_args()

    gltf = GLTF2().load(args.input)
    blob = bytearray(gltf.binary_blob())
    if len(gltf.skins) != 1:
        sys.exit(f"expected exactly one skin, found {len(gltf.skins)} (the loader always uses skin 0)")
    skin = gltf.skins[0]

    # Face node: explicit, else the unique node carrying the skin.
    if args.face_node:
        face_node = node_by_name(gltf, args.face_node)
        if face_node is None:
            sys.exit(f"face node '{args.face_node}' not found")
    else:
        # The face is the skinned mesh that carries the facial morph targets;
        # skinned accessories (hearing aids etc.) have none.
        skinned = [i for i, n in enumerate(gltf.nodes) if n.skin is not None and n.mesh is not None
                   and any(p.targets for p in gltf.meshes[n.mesh].primitives)]
        if len(skinned) != 1:
            sys.exit(f"could not auto-detect the face node (skinned+morphed mesh nodes: {skinned}); pass --face-node")
        face_node = skinned[0]
    face_mesh = gltf.nodes[face_node].mesh
    log(f"face node: '{gltf.nodes[face_node].name}' (mesh {face_mesh})")

    def joint_index(name):
        ni = node_by_name(gltf, name)
        if ni is None or ni not in skin.joints:
            sys.exit(f"joint '{name}' not found in the skin")
        return skin.joints.index(ni)
    jaw_j, head_j = joint_index(args.jaw), joint_index(args.head)

    # ---- classify every other mesh node ----
    overrides = {}
    for spec in args.bind:
        n, _, j = spec.partition("=")
        if j not in ("jaw", "head"):
            sys.exit(f"--bind {spec}: joint must be jaw or head")
        overrides[n] = j

    mouth_nodes = {}   # node idx -> joint local index
    extra_meshes = []  # already-skinned accessories (kept verbatim, in mesh order)
    drop_meshes = set()
    for i, n in enumerate(gltf.nodes):
        if n.mesh is None or i == face_node:
            continue
        name = n.name or ""
        if any(name.startswith(p) for p in args.drop):
            drop_meshes.add(n.mesh)
            log(f"dropping '{name}'")
            continue
        # Mouth meshes are recognised by name, whether or not they are already
        # skinned — a re-export of a previously rigged GLB keeps the injected
        # skinning, and it is simply redone. Anything else that is skinned is an
        # accessory (hearing aid etc.) and is kept verbatim.
        if name in overrides:
            j = overrides[name]
        elif "Lower" in name or name == "Tongue":
            j = "jaw"
        elif "Upper" in name or name == "Uvula":
            j = "head"
        elif n.skin is not None:
            extra_meshes.append(n.mesh)
            log(f"keeping skinned accessory '{name}' as-is")
            continue
        else:
            sys.exit(f"cannot tell whether '{name}' follows the jaw or the head; pass --bind {name}=jaw|head")
        mouth_nodes[i] = jaw_j if j == "jaw" else head_j

    if not mouth_nodes:
        sys.exit("no mouth meshes found")

    # ---- per mouth mesh: bake transform, inject skin, classify cavity ----
    parent_of = build_parent_map(gltf)
    face_world_inv = np.linalg.inv(node_world_matrix(gltf, face_node, parent_of))
    cavity_meshes = set()
    for ni, joint in mouth_nodes.items():
        node = gltf.nodes[ni]
        mesh = gltf.meshes[node.mesh]
        if len(mesh.primitives) != 1:
            sys.exit(f"'{node.name}': expected 1 primitive, found {len(mesh.primitives)}")
        prim = mesh.primitives[0]
        reskin = prim.attributes.JOINTS_0 is not None  # previous rig's data; replaced below

        rel = face_world_inv @ node_world_matrix(gltf, ni, parent_of)
        pos, pstart = read_vec3(gltf, blob, prim.attributes.POSITION)
        pos_h = np.hstack([pos, np.ones((len(pos), 1))]) @ rel.T
        write_vec3(blob, pstart, pos_h[:, :3])
        acc = gltf.accessors[prim.attributes.POSITION]
        acc.min = pos_h[:, :3].min(axis=0).tolist()
        acc.max = pos_h[:, :3].max(axis=0).tolist()
        if prim.attributes.NORMAL is not None:
            nrm, nstart = read_vec3(gltf, blob, prim.attributes.NORMAL)
            nrm = nrm @ np.linalg.inv(rel[:3, :3])  # row-vector form of inverse-transpose
            nrm /= np.linalg.norm(nrm, axis=1, keepdims=True) + 1e-12
            write_vec3(blob, nstart, nrm)
        node.translation = node.rotation = node.scale = node.matrix = None

        count = acc.count
        j_idx = append_accessor(gltf, blob, struct.pack(f"<{count * 4}B", *([joint, 0, 0, 0] * count)),
                                pygltflib.UNSIGNED_BYTE, "VEC4", count)
        w_idx = append_accessor(gltf, blob, struct.pack(f"<{count * 4}f", *([1.0, 0.0, 0.0, 0.0] * count)),
                                pygltflib.FLOAT, "VEC4", count)
        prim.attributes.JOINTS_0 = j_idx
        prim.attributes.WEIGHTS_0 = w_idx
        node.skin = 0

        inward = fraction_inward(pos_h[:, :3], read_indices(gltf, blob, prim.indices))
        is_cavity = inward > args.cavity_threshold
        if is_cavity:
            cavity_meshes.add(node.mesh)
        log(f"  {node.name}: {count} verts -> {'jaw' if joint == jaw_j else 'head'}"
            f"{', cavity shell' if is_cavity else ''}{', re-skinned' if reskin else ''} (inward {inward:.2f})")

    # ---- materials: 0=face, 1=teeth, 2=cavity (textures stripped) ----
    face_mat_idx = gltf.meshes[face_mesh].primitives[0].material
    # A fresh export has one shared teeth material; a re-export of a rigged GLB
    # also carries the cavity copy. Either way the blocks are rebuilt below, so
    # any mouth material works as the template.
    mouth_mat_idx = sorted({gltf.meshes[gltf.nodes[ni].mesh].primitives[0].material for ni in mouth_nodes})
    teeth_src = gltf.materials[mouth_mat_idx[0]]
    face_m = strip_textures(copy.deepcopy(gltf.materials[face_mat_idx]))
    teeth_m = strip_textures(copy.deepcopy(teeth_src))
    cavity_m = strip_textures(copy.deepcopy(teeth_src))
    teeth_m.name = "M_Teeth"
    cavity_m.name = "M_Teeth_Cavity"
    new_materials = [face_m, teeth_m, cavity_m]
    # Accessory materials keep their identity; slot = position in the new list.
    extra_slot_of_old_mat = {}
    extra_meshes = sorted(set(extra_meshes))
    for mi in extra_meshes:
        for p in gltf.meshes[mi].primitives:
            if p.material not in extra_slot_of_old_mat:
                extra_slot_of_old_mat[p.material] = len(new_materials)
                new_materials.append(strip_textures(copy.deepcopy(gltf.materials[p.material])))
    old_materials = gltf.materials
    gltf.materials = new_materials
    gltf.images, gltf.textures = [], []
    gltf.extensionsUsed = [e for e in (gltf.extensionsUsed or []) if not e.startswith("KHR_materials_")]
    gltf.extensionsRequired = [e for e in (gltf.extensionsRequired or []) if not e.startswith("KHR_materials_")]

    for mi, m in enumerate(gltf.meshes):
        for p in m.primitives:
            if mi == face_mesh:
                p.material = 0
            elif mi in extra_meshes:
                p.material = extra_slot_of_old_mat[p.material]
            else:
                p.material = 2 if mi in cavity_meshes else 1

    # ---- mesh order: face first, then mouth meshes, then accessories; drop helpers ----
    keep = [face_mesh] + sorted(gltf.nodes[ni].mesh for ni in mouth_nodes) + extra_meshes
    remap = {old: new for new, old in enumerate(keep)}
    gltf.meshes = [gltf.meshes[i] for i in keep]
    for n in gltf.nodes:
        if n.mesh is not None:
            n.mesh = remap.get(n.mesh)  # dropped meshes -> None
            if n.mesh is None:
                n.skin = None

    saved = compact_buffer(gltf, blob)
    gltf.save(args.out)
    log(f"wrote {args.out} ({Path(args.out).stat().st_size / 1e6:.1f} MB, {saved / 1e6:.1f} MB of embedded data dropped)")

    # ---- recipe ----
    prim_mats = [gltf.meshes[remap[i]].primitives[0].material for i in keep]
    if args.recipe:
        recipe = json.loads(Path(args.recipe).read_text())
        if "material" not in recipe:
            sys.exit(f"{args.recipe} has no 'material' block to use as the face")
        extras_blocks = [teeth_material_block(), cavity_material_block()]
        if len(new_materials) > 3:
            # Accessory blocks come from the previous bake (by material name).
            if not args.old_glb:
                sys.exit(f"accessory materials {[m.name for m in new_materials[3:]]} need their engine "
                         f"blocks; pass --old-glb <previous baked glb>")
            old = GLTF2().load(args.old_glb)
            by_name = {m.name: (m.extras or {}).get("vkfw_material") for m in old.materials}
            for m in new_materials[3:]:
                block = by_name.get(m.name)
                if not block:
                    sys.exit(f"'{m.name}' has no baked block in {args.old_glb}; add it to the recipe by hand")
                if "$GLB[" in block:
                    sys.exit(f"'{m.name}' block references embedded textures; add it to the recipe by hand "
                             f"with resources/ texture paths")
                extras_blocks.append(json.loads(block))
        recipe["extra_materials"] = extras_blocks
        recipe["primitive_materials"] = prim_mats
        out = Path(args.out_recipe or args.recipe)
        out.write_text(json.dumps(recipe, indent=2) + "\n")
        log(f"wrote recipe {out}  (primitive_materials = {prim_mats})")
    else:
        log(f"no --recipe given; slots are 0=face 1=teeth 2=cavity, primitive_materials = {prim_mats}")


if __name__ == "__main__":
    main()
