#!/usr/bin/env python3
"""
fht_npy_to_json.py — Convert a FLAME-Head-Tracker (FHT) per-frame .npy
sequence into the animation JSON format consumed by this engine (see
ANIMATION.md).

FHT writes one .npy per frame (typically NNNNN.npy inside a folder),
each pickling a dict with at least these keys:

    exp        (1, 100) float32   - SMPL-X expression coefficients
    head_pose  (1,   3) float32   - head joint axis-angle
    jaw_pose   (1,   3) float32   - jaw  joint axis-angle
    eye_pose   (1,   6) float32   - left (0:3) + right (3:6) eye axis-angle

(Other fields — shape, tex, light, cam, K, img*, parsing, lmks*,
blendshape_scores — are FHT diagnostic outputs and are ignored.)

The Alex GLB rig is a verbatim SMPL-X skeleton, and morph targets are
named `Exp_000` ... `Exp_099` (one per expression coefficient), so this
is a direct 1:1 retarget: no SMPL-X model, betas, torch or smplx package
needed. Only numpy is required.

Usage:
    python3 tools/fht_npy_to_json.py INPUT [OUTPUT] [options]

    INPUT   a folder containing NNNNN.npy frames, OR a parent folder
            holding multiple such sequences (batch mode; one .json per
            sub-folder that contains .npy files).
    OUTPUT  optional. Single mode: the .json path (or a dir to write
            into). Batch mode: the output directory (structure mirrored).
            Defaults to writing <sequence_dir>.json next to the source.

Options:
    --fps N                   output sample rate (default: 30)
    --loop                    set "loop": true (default: false)
    --keep-static             keep tracks that never leave the rest pose
                              (default: identity-only tracks are dropped)
    --no-head-pose            do not emit joint:head/rotation
    --no-jaw-pose             do not emit joint:jaw/rotation
    --no-eye-pose             do not emit left_eye/right_eye rotations
    --no-expressions          do not emit any morph:Exp_NNN track
    --num-expressions N       number of expression coefficients to emit
                              (default: 100; capped at the array length)
    --name NAME               override the animation `name` field
"""

import argparse
import glob
import json
import math
import os
import re
import sys

import numpy as np


# Keys we read from each .npy. Missing keys are silently treated as zero.
FHT_KEYS = ("exp", "head_pose", "jaw_pose", "eye_pose")


def aa_to_quat(aa):
    """Axis-angle (..., 3) -> quaternion (..., 4) in [x, y, z, w] order."""
    aa = np.asarray(aa, dtype=np.float64)
    angle = np.linalg.norm(aa, axis=-1, keepdims=True)
    small = angle < 1e-8
    safe = np.where(small, 1.0, angle)
    axis = aa / safe
    half = angle * 0.5
    xyz = axis * np.sin(half)
    w = np.cos(half)
    q = np.concatenate([xyz, w], axis=-1)
    ident = np.array([0.0, 0.0, 0.0, 1.0])
    return np.where(small, ident, q)


def num(x):
    """Compact number string: up to 6 decimals, trailing zeros trimmed."""
    s = f"{x:.6f}".rstrip("0").rstrip(".")
    return "0" if s in ("", "-0") else s


def is_static_rotation(quats, eps=1e-3):
    """True if every quaternion is within `eps` radians of identity."""
    w = np.clip(np.abs(quats[:, 3]), 0.0, 1.0)
    angle = 2.0 * np.arccos(w)
    return bool(np.all(angle < eps))


def is_static_scalar(values, eps=1e-4):
    """True if all values are within `eps` of zero."""
    return bool(np.max(np.abs(values)) < eps)


_FRAME_RE = re.compile(r"^(\d+)\.npy$", re.IGNORECASE)


def list_frame_files(folder):
    """Return sorted .npy files in `folder`, ordered by numeric stem when
    the names are numeric (00000.npy, 00001.npy, ...), else lexicographic."""
    files = glob.glob(os.path.join(folder, "*.npy"))
    if not files:
        return []
    keyed = []
    all_numeric = True
    for f in files:
        m = _FRAME_RE.match(os.path.basename(f))
        if m:
            keyed.append((int(m.group(1)), f))
        else:
            all_numeric = False
            break
    if all_numeric:
        keyed.sort()
        return [f for _, f in keyed]
    return sorted(files)


def load_frame(path):
    """Read one FHT .npy and return a dict with just the pose fields,
    each as a flat float64 array (zero-filled if the key is absent)."""
    raw = np.load(path, allow_pickle=True).item()
    out = {}
    for k in FHT_KEYS:
        if k in raw:
            out[k] = np.asarray(raw[k], dtype=np.float64).reshape(-1)
        else:
            out[k] = None
    return out


def stack_sequence(files):
    """Walk `files` in order and stack pose fields into per-frame arrays.

    Returns a dict:
        exp        (N, M)  - M is the smallest 'exp' length seen (>=0)
        head_pose  (N, 3)
        jaw_pose   (N, 3)
        eye_pose   (N, 6)
    Missing fields in any frame become zeros for that frame.
    """
    n = len(files)
    # First pass: discover expression length from the first frame that has it.
    exp_dim = 0
    for f in files:
        first = load_frame(f)
        if first["exp"] is not None:
            exp_dim = first["exp"].shape[0]
            break

    exp = np.zeros((n, exp_dim), dtype=np.float64)
    head = np.zeros((n, 3), dtype=np.float64)
    jaw = np.zeros((n, 3), dtype=np.float64)
    eye = np.zeros((n, 6), dtype=np.float64)

    for i, f in enumerate(files):
        d = load_frame(f)
        if exp_dim and d["exp"] is not None:
            v = d["exp"]
            k = min(exp_dim, v.shape[0])
            exp[i, :k] = v[:k]
        if d["head_pose"] is not None:
            head[i, :3] = d["head_pose"][:3]
        if d["jaw_pose"] is not None:
            jaw[i, :3] = d["jaw_pose"][:3]
        if d["eye_pose"] is not None:
            v = d["eye_pose"]
            k = min(6, v.shape[0])
            eye[i, :k] = v[:k]

    return {"exp": exp, "head_pose": head, "jaw_pose": jaw, "eye_pose": eye}


def build_tracks(seq, opts):
    """Return a list of (target, keys_list) tuples. `keys_list` is a list
    of pre-formatted strings (one per keyframe) ready to embed in the
    JSON body."""
    n = seq["head_pose"].shape[0]
    fps = float(opts["fps"])
    times = np.arange(n, dtype=np.float64) / fps

    tracks = []

    # ---- Joint rotation tracks (axis-angle -> quaternion) ----------------
    joint_specs = []
    if opts["head"]:
        joint_specs.append(("head", seq["head_pose"]))
    if opts["jaw"]:
        joint_specs.append(("jaw", seq["jaw_pose"]))
    if opts["eye"]:
        joint_specs.append(("left_eye", seq["eye_pose"][:, 0:3]))
        joint_specs.append(("right_eye", seq["eye_pose"][:, 3:6]))

    for joint, aa in joint_specs:
        q = aa_to_quat(aa)
        if not opts["keep_static"] and is_static_rotation(q):
            continue
        keys = [f"[{num(tm)}, [{num(v[0])}, {num(v[1])}, {num(v[2])}, {num(v[3])}]]"
                for tm, v in zip(times, q)]
        tracks.append((f"joint:{joint}/rotation", keys))

    # ---- Morph weight tracks (Exp_NNN) -----------------------------------
    if opts["expressions"] and seq["exp"].shape[1] > 0:
        cap = min(opts["num_expressions"], seq["exp"].shape[1])
        for j in range(cap):
            vals = seq["exp"][:, j]
            if not opts["keep_static"] and is_static_scalar(vals):
                continue
            keys = [f"[{num(tm)}, {num(v)}]" for tm, v in zip(times, vals)]
            tracks.append((f"morph:Exp_{j:03d}", keys))

    return tracks, float(times[-1]) if n else 0.0


def write_animation(out_path, name, duration, fps, loop, tracks):
    parts = []
    for target, keys in tracks:
        body_str = ",\n        ".join(keys)
        parts.append(
            '    {\n'
            f'      "target": "{target}",\n'
            '      "interp": "linear",\n'
            '      "keys": [\n'
            f'        {body_str}\n'
            '      ]\n'
            '    }'
        )
    doc = (
        '{\n'
        f'  "name": "{name}",\n'
        f'  "duration": {num(duration)},\n'
        f'  "fps": {num(float(fps))},\n'
        f'  "loop": {"true" if loop else "false"},\n'
        '  "tracks": [\n'
        + ",\n".join(parts) +
        '\n  ]\n'
        '}\n'
    )
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write(doc)


def convert_sequence(in_dir, out_path, name, opts):
    files = list_frame_files(in_dir)
    if not files:
        raise ValueError(f"no .npy frames in {in_dir}")
    seq = stack_sequence(files)
    tracks, duration = build_tracks(seq, opts)
    write_animation(out_path, name, duration, opts["fps"], opts["loop"], tracks)
    print(f"  {os.path.basename(in_dir.rstrip(os.sep))}: "
          f"{len(files)} frames -> {len(tracks)} tracks, "
          f"{duration:.2f}s @ {opts['fps']:g}fps -> {out_path}")


def find_sequence_dirs(root):
    """Locate every folder under `root` (inclusive) that directly contains
    at least one .npy file. The list is sorted for stable batch output."""
    found = []
    for dirpath, _dirs, files in os.walk(root):
        if any(f.lower().endswith(".npy") for f in files):
            found.append(dirpath)
    return sorted(found)


def resolve_single_output(in_dir, out_arg):
    name = os.path.basename(os.path.normpath(in_dir)) or "anim"
    if out_arg is None:
        parent = os.path.dirname(os.path.abspath(os.path.normpath(in_dir)))
        return os.path.join(parent, name + ".json")
    if os.path.isdir(out_arg) or out_arg.endswith(("/", os.sep)):
        return os.path.join(out_arg, name + ".json")
    return out_arg


def main():
    ap = argparse.ArgumentParser(
        description="Convert FHT per-frame .npy sequences into engine animation JSON.")
    ap.add_argument("input", help="folder of .npy frames OR parent folder (batch)")
    ap.add_argument("output", nargs="?", default=None,
                    help="output .json (or directory). Default: alongside input")
    ap.add_argument("--fps", type=float, default=30.0,
                    help="output sample rate (default: 30)")
    ap.add_argument("--loop", action="store_true",
                    help='set "loop": true (default false)')
    ap.add_argument("--keep-static", action="store_true",
                    help="keep tracks that never leave the rest pose")
    ap.add_argument("--no-head-pose", action="store_true",
                    help="skip joint:head/rotation")
    ap.add_argument("--no-jaw-pose", action="store_true",
                    help="skip joint:jaw/rotation")
    ap.add_argument("--no-eye-pose", action="store_true",
                    help="skip joint:left_eye/right_eye rotations")
    ap.add_argument("--no-expressions", action="store_true",
                    help="skip every morph:Exp_NNN track")
    ap.add_argument("--num-expressions", type=int, default=100,
                    help="number of expression coefficients to emit (default 100)")
    ap.add_argument("--name", default=None,
                    help="override animation name (default: folder name)")
    args = ap.parse_args()

    if not os.path.isdir(args.input):
        ap.error(f"input must be a directory: {args.input}")

    opts = dict(
        fps=args.fps,
        loop=args.loop,
        keep_static=args.keep_static,
        head=not args.no_head_pose,
        jaw=not args.no_jaw_pose,
        eye=not args.no_eye_pose,
        expressions=not args.no_expressions,
        num_expressions=max(0, args.num_expressions),
    )

    # Single sequence: .npy files sit directly under INPUT.
    direct = list_frame_files(args.input)
    if direct:
        out_path = resolve_single_output(args.input, args.output)
        nm = args.name or os.path.basename(os.path.normpath(args.input)) or "anim"
        convert_sequence(args.input, out_path, nm, opts)
        return

    # Batch: scan for subfolders that contain .npy frames.
    seq_dirs = find_sequence_dirs(args.input)
    if not seq_dirs:
        ap.error(f"no .npy frames found under {args.input}")

    out_root = args.output
    print(f"Batch: {len(seq_dirs)} sequence(s) under {args.input}")
    ok = 0
    for d in seq_dirs:
        rel = os.path.relpath(d, args.input)
        # If the sequence lives in a 'npy' subfolder (FHT's default), use
        # its parent's name as the sequence label so the output is
        # <parent>.json instead of npy.json.
        if os.path.basename(d).lower() == "npy":
            label = os.path.basename(os.path.dirname(d)) or "anim"
            rel_label = os.path.dirname(rel) or label
        else:
            label = os.path.basename(d) or "anim"
            rel_label = rel
        if out_root:
            out_path = os.path.join(out_root, rel_label + ".json")
        else:
            out_path = os.path.join(os.path.dirname(os.path.abspath(d)),
                                    label + ".json")
        nm = args.name or label
        try:
            convert_sequence(d, out_path, nm, opts)
            ok += 1
        except Exception as e:
            print(f"  SKIP {rel}: {e}", file=sys.stderr)
    print(f"Done: {ok}/{len(seq_dirs)} converted.")


if __name__ == "__main__":
    main()
