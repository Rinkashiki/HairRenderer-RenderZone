#!/usr/bin/env python3
"""
offset_glb_meshes.py — translate the vertices of selected meshes inside a GLB,
in place, without touching anything else in the file.

Meant for small placement fixes on the baked character GLBs (e.g. "move the
teeth up by 5 mm") that would otherwise need the whole Blender re-export →
rig_teeth_glb.py → bake_glb_material.py round trip. The character mouth meshes
are skinned 100 % to a single joint, so shifting their bind-pose POSITION data
is exactly what moving the object in Blender does (rest-pose joint × IBM is the
identity, so the skinned result moves by the same vector). Axes are glTF's:
+Y up, i.e. Blender +Z.

Only the matched meshes' POSITION accessors change (data + min/max); textures,
skins, morph targets (deltas — unaffected by a translation), materials and
the binary layout stay byte-identical, so the file stays LFS-friendly in size.

    python tools/offset_glb_meshes.py resources/models/*/*.glb --offset 0 0.005 0
    python tools/offset_glb_meshes.py maria.glb --offset 0 0.005 0 --meshes Teeth_Upper Teeth_Lower
    python tools/offset_glb_meshes.py maria.glb --offset 0 0.005 0 --out /tmp/maria.shifted.glb

Meshes are matched by NODE name (the names Blender shows), as in rig_teeth_glb.py.
Standard library only.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

MOUTH_NODES = ("Teeth_Upper", "Teeth_Lower", "Teeth_Interior_Upper", "Teeth_Interior_Lower",
               "MouthCavity_Upper", "MouthCavity_Lower", "Tongue", "Uvula")

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
FLOAT = 5126


def read_glb(path):
    data = Path(path).read_bytes()
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2:
        sys.exit(f"{path}: not a glTF 2.0 binary")
    off = 12
    gltf = blob = None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        if ctype == CHUNK_JSON:
            gltf = json.loads(data[off:off + clen])
        elif ctype == CHUNK_BIN:
            blob = bytearray(data[off:off + clen])
        off += clen
    if gltf is None or blob is None:
        sys.exit(f"{path}: missing JSON or BIN chunk")
    return gltf, blob


def write_glb(path, gltf, blob):
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    while len(blob) % 4:
        blob.append(0)
    total = 12 + 8 + len(js) + 8 + len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", GLB_MAGIC, 2, total))
        f.write(struct.pack("<II", len(js), CHUNK_JSON))
        f.write(js)
        f.write(struct.pack("<II", len(blob), CHUNK_BIN))
        f.write(blob)


def offset_accessor(gltf, blob, acc_idx, offset):
    acc = gltf["accessors"][acc_idx]
    if acc.get("componentType") != FLOAT or acc.get("type") != "VEC3" or "sparse" in acc:
        sys.exit(f"accessor {acc_idx}: expected dense float VEC3 positions")
    bv = gltf["bufferViews"][acc["bufferView"]]
    if bv.get("buffer", 0) != 0:
        sys.exit(f"accessor {acc_idx}: positions must live in the GLB-embedded buffer")
    stride = bv.get("byteStride", 12)
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for i in range(acc["count"]):
        at = start + i * stride
        v = [c + o for c, o in zip(struct.unpack_from("<3f", blob, at), offset)]
        struct.pack_into("<3f", blob, at, *v)
        for k in range(3):
            lo[k] = min(lo[k], v[k])
            hi[k] = max(hi[k], v[k])
    # Recompute from the written float32 data so min/max match the buffer exactly.
    acc["min"], acc["max"] = lo, hi
    return acc["count"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("glb", nargs="+", help="GLB file(s) to patch")
    ap.add_argument("--offset", nargs=3, type=float, metavar=("X", "Y", "Z"), required=True,
                    help="translation to add, glTF axes (+Y up)")
    ap.add_argument("--meshes", nargs="+", default=list(MOUTH_NODES), metavar="NODE",
                    help=f"node names to move (default: the mouth model — {' '.join(MOUTH_NODES)})")
    ap.add_argument("--out", help="write here instead of in place (single input only)")
    ap.add_argument("--dry-run", action="store_true", help="report what would move, write nothing")
    args = ap.parse_args()
    if args.out and len(args.glb) != 1:
        sys.exit("--out needs exactly one input GLB")

    wanted = set(args.meshes)
    for path in args.glb:
        gltf, blob = read_glb(path)
        by_name = {}
        for n in gltf.get("nodes", []):
            if "mesh" in n and n.get("name") in wanted:
                by_name.setdefault(n["name"], []).append(n["mesh"])
        missing = sorted(wanted - by_name.keys())
        if missing:
            sys.exit(f"{path}: no mesh node named {missing} (have: "
                     f"{sorted(n.get('name', '?') for n in gltf['nodes'] if 'mesh' in n)})")

        print(f"[offset] {path}: +{tuple(args.offset)}")
        done = set()  # an accessor shared by two primitives must only move once
        for name in args.meshes:
            for mi in by_name[name]:
                for prim in gltf["meshes"][mi]["primitives"]:
                    acc = prim["attributes"]["POSITION"]
                    if acc in done:
                        continue
                    done.add(acc)
                    before = list(gltf["accessors"][acc].get("min", ["?"] * 3))
                    count = offset_accessor(gltf, blob, acc, args.offset)
                    after = gltf["accessors"][acc]["min"]
                    print(f"  {name:22} mesh {mi:2}  {count:6} verts  minY {before[1]:.5f} -> {after[1]:.5f}")
        if args.dry_run:
            print("  (dry run — nothing written)")
            continue
        out = args.out or path
        write_glb(out, gltf, blob)
        print(f"  wrote {out}")


if __name__ == "__main__":
    main()
