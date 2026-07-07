#!/usr/bin/env python3
"""
amass_to_json.py — Convert AMASS / SMPL-X .npz motion capture into the
animation JSON format consumed by this engine (see ANIMATION.md).

The Alex GLB rig is a verbatim SMPL-X 55-joint skeleton (identical joint
names and ordering), so this is a direct skeletal retarget: each SMPL-X
axis-angle pose block maps 1:1 onto a rig joint. No SMPL-X model, identity
betas, torch or smplx package is needed — the engine does the skinning;
this script only emits per-joint rotation/translation keyframes.

Usage:
    python3 tools/amass_to_json.py INPUT [OUTPUT] [options]

    INPUT   an AMASS .npz file, or a directory (batch: all *.npz, recursive)
    OUTPUT  optional. For a single file: the .json path (or a dir to write
            into). For a directory INPUT: the output directory (structure
            mirrored). Defaults to the .json sitting next to each source.

Options:
    --channels body,hands,face   comma list of channel groups to emit
                                 (default: body,hands,face)
    --fps N                      output sample rate in fps (default: 30)
    --no-root-motion             drop the pelvis translation track
                                 (default: root motion is emitted)
    --no-up-convert              do not apply the Z-up -> Y-up rotation
                                 (default: it IS applied at the root)
    --loop                       set "loop": true (default: false; AMASS
                                 clips are one-shot mocap)
    --keep-static                keep rotation tracks that never move
                                 (default: identity-only tracks are dropped)
    --name NAME                  override the animation name (default: the
                                 input file stem)
"""

import argparse
import json
import math
import os
import sys
import glob

import numpy as np

# ---------------------------------------------------------------------------
# SMPL-X 55-joint kinematic tree — identical to the Alex GLB joint ordering
# (verified against resources/models/alex/alex.glb.params.txt).
# ---------------------------------------------------------------------------
SMPLX_JOINT_NAMES = [
    "pelvis",                                                       # 0   root
    "left_hip", "right_hip", "spine1", "left_knee", "right_knee",   # 1-5
    "spine2", "left_ankle", "right_ankle", "spine3", "left_foot",   # 6-10
    "right_foot", "neck", "left_collar", "right_collar", "head",    # 11-15
    "left_shoulder", "right_shoulder", "left_elbow", "right_elbow", # 16-19
    "left_wrist", "right_wrist",                                    # 20-21  (body ends)
    "jaw", "left_eye", "right_eye",                                 # 22-24  (face)
    "left_index1", "left_index2", "left_index3",                    # 25-27
    "left_middle1", "left_middle2", "left_middle3",                 # 28-30
    "left_pinky1", "left_pinky2", "left_pinky3",                    # 31-33
    "left_ring1", "left_ring2", "left_ring3",                       # 34-36
    "left_thumb1", "left_thumb2", "left_thumb3",                    # 37-39
    "right_index1", "right_index2", "right_index3",                 # 40-42
    "right_middle1", "right_middle2", "right_middle3",              # 43-45
    "right_pinky1", "right_pinky2", "right_pinky3",                 # 46-48
    "right_ring1", "right_ring2", "right_ring3",                    # 49-51
    "right_thumb1", "right_thumb2", "right_thumb3",                 # 52-54
]

# Channel group -> joint indices into SMPLX_JOINT_NAMES.
GROUP_BODY = list(range(1, 22))            # 21 body joints (pelvis handled separately)
GROUP_FACE = [22, 23, 24]                  # jaw, left_eye, right_eye
GROUP_HANDS = list(range(25, 55))          # 30 finger joints

# Z-up (SMPL-X / AMASS world) -> Y-up (glTF) is a -90 deg rotation about X.
# As a [x, y, z, w] quaternion:
_S = math.sin(-math.pi / 4.0)
_C = math.cos(-math.pi / 4.0)
Q_UP_CONVERT = np.array([_S, 0.0, 0.0, _C], dtype=np.float64)


def aa_to_quat(aa):
    """Axis-angle (..., 3) -> quaternion (..., 4) in [x, y, z, w] order."""
    aa = np.asarray(aa, dtype=np.float64)
    angle = np.linalg.norm(aa, axis=-1, keepdims=True)          # (..., 1)
    small = angle < 1e-8
    safe = np.where(small, 1.0, angle)
    axis = aa / safe                                            # (..., 3)
    half = angle * 0.5
    xyz = axis * np.sin(half)
    w = np.cos(half)
    q = np.concatenate([xyz, w], axis=-1)                       # (..., 4)
    ident = np.array([0.0, 0.0, 0.0, 1.0])
    return np.where(small, ident, q)


def quat_mul(a, b):
    """Hamilton product a (x) b, both (..., 4) in [x, y, z, w]."""
    ax, ay, az, aw = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    bx, by, bz, bw = b[..., 0], b[..., 1], b[..., 2], b[..., 3]
    return np.stack([
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ], axis=-1)


def num(x):
    """Compact number string: up to 6 decimals, trailing zeros trimmed."""
    s = f"{x:.6f}".rstrip("0").rstrip(".")
    return "0" if s in ("", "-0") else s


def extract_pose_blocks(npz):
    """Return (trans, root_orient, body, jaw, eye, hand, n_frames) as
    (N,3)/(N,63)/(N,3)/(N,6)/(N,90) float arrays. Prefers AMASS split
    arrays; falls back to slicing the SMPL-X `poses` (165) layout."""
    if "trans" not in npz:
        raise ValueError("no 'trans' array — not an AMASS motion file")
    trans = np.asarray(npz["trans"], dtype=np.float64)
    n = trans.shape[0]

    if "root_orient" in npz and "pose_body" in npz:
        root = np.asarray(npz["root_orient"], dtype=np.float64)
        body = np.asarray(npz["pose_body"], dtype=np.float64)
        jaw = np.asarray(npz["pose_jaw"], dtype=np.float64) if "pose_jaw" in npz else np.zeros((n, 3))
        eye = np.asarray(npz["pose_eye"], dtype=np.float64) if "pose_eye" in npz else np.zeros((n, 6))
        hand = np.asarray(npz["pose_hand"], dtype=np.float64) if "pose_hand" in npz else np.zeros((n, 90))
    elif "poses" in npz:
        p = np.asarray(npz["poses"], dtype=np.float64)          # (N, 165)
        root = p[:, 0:3]
        body = p[:, 3:66]
        jaw = p[:, 66:69]
        eye = p[:, 69:75]
        hand = p[:, 75:165]
    else:
        raise ValueError("no recognizable pose arrays (need root_orient+pose_body or poses)")

    return trans, root, body, jaw, eye, hand, n


def joint_local_quats(body, jaw, eye, hand, n):
    """Build an (N, 55, 4) array of per-joint LOCAL quaternions.
    Index 0 (pelvis) is left as identity here; the root rotation is
    handled separately so the up-axis conversion can be applied to it."""
    q = np.zeros((n, 55, 4), dtype=np.float64)
    q[:, 0] = np.array([0.0, 0.0, 0.0, 1.0])
    q[:, 1:22] = aa_to_quat(body.reshape(n, 21, 3))
    q[:, 22] = aa_to_quat(jaw.reshape(n, 3))
    q[:, 23] = aa_to_quat(eye[:, 0:3])
    q[:, 24] = aa_to_quat(eye[:, 3:6])
    q[:, 25:55] = aa_to_quat(hand.reshape(n, 30, 3))
    return q


def is_static_rotation(quats, eps=1e-3):
    """True if every quaternion is within `eps` radians of identity."""
    w = np.clip(np.abs(quats[:, 3]), 0.0, 1.0)
    angle = 2.0 * np.arccos(w)
    return bool(np.all(angle < eps))


def convert_file(in_path, out_path, channels, out_fps, root_motion,
                 up_convert, loop, keep_static, name):
    npz = np.load(in_path, allow_pickle=True)
    trans, root_aa, body, jaw, eye, hand, n = extract_pose_blocks(npz)

    src_fps = float(npz["mocap_frame_rate"]) if "mocap_frame_rate" in npz else 120.0
    if src_fps <= 0:
        src_fps = 120.0

    # Decimate to the requested output rate while keeping true wall-clock
    # timestamps (preserves real-world playback speed).
    stride = max(1, int(round(src_fps / float(out_fps))))
    idx = np.arange(0, n, stride)
    times = idx / src_fps
    duration = float(times[-1]) if len(times) else 0.0

    qlocal = joint_local_quats(body, jaw, eye, hand, n)         # (N,55,4)

    # Root (pelvis) rotation, with optional Z-up -> Y-up frame change.
    qroot = aa_to_quat(root_aa)                                 # (N,4)
    if up_convert:
        qroot = quat_mul(np.broadcast_to(Q_UP_CONVERT, qroot.shape), qroot)
    qlocal[:, 0] = qroot

    # Which joints to emit, by selected channel groups.
    sel = set()
    if "body" in channels:
        sel.update([0] + GROUP_BODY)
    if "face" in channels:
        sel.update(GROUP_FACE)
    if "hands" in channels:
        sel.update(GROUP_HANDS)

    tracks = []

    # pelvis translation track (full root motion).
    if 0 in sel and root_motion:
        t = trans[idx]
        if up_convert:                                          # (x,y,z) -> (x,z,-y)
            t = np.stack([t[:, 0], t[:, 2], -t[:, 1]], axis=-1)
        keys = [f"[{num(tm)}, [{num(v[0])}, {num(v[1])}, {num(v[2])}]]"
                for tm, v in zip(times, t)]
        tracks.append(("joint:pelvis/translation", keys))

    # rotation tracks, one per selected joint.
    for j in sorted(sel):
        q = qlocal[idx, j]                                      # (K,4)
        if not keep_static and is_static_rotation(q):
            continue
        keys = [f"[{num(tm)}, [{num(v[0])}, {num(v[1])}, {num(v[2])}, {num(v[3])}]]"
                for tm, v in zip(times, q)]
        tracks.append((f"joint:{SMPLX_JOINT_NAMES[j]}/rotation", keys))

    # Assemble JSON by hand so each keyframe stays on a single line
    # (matches resources/animations/*.json style, keeps files compact).
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
        f'  "fps": {num(float(out_fps))},\n'
        f'  "loop": {"true" if loop else "false"},\n'
        '  "tracks": [\n'
        + ",\n".join(parts) +
        '\n  ]\n'
        '}\n'
    )

    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write(doc)

    print(f"  {os.path.basename(in_path)}: {n} src frames @ {src_fps:g}fps "
          f"-> {len(idx)} keys @ {out_fps:g}fps, {len(tracks)} tracks, "
          f"{duration:.2f}s -> {out_path}")


def resolve_single_output(in_path, out_arg):
    stem = os.path.splitext(os.path.basename(in_path))[0]
    if out_arg is None:
        return os.path.join(os.path.dirname(os.path.abspath(in_path)), stem + ".json")
    if os.path.isdir(out_arg) or out_arg.endswith(("/", os.sep)):
        return os.path.join(out_arg, stem + ".json")
    return out_arg


def main():
    ap = argparse.ArgumentParser(
        description="Convert AMASS/SMPL-X .npz motion into engine animation JSON.")
    ap.add_argument("input", help="AMASS .npz file or a directory (batch)")
    ap.add_argument("output", nargs="?", default=None,
                    help="output .json (or directory). Default: next to input")
    ap.add_argument("--channels", default="body,hands,face",
                    help="comma list of: body,hands,face (default: all)")
    ap.add_argument("--fps", type=float, default=30.0,
                    help="output sample rate (default: 30)")
    ap.add_argument("--no-root-motion", action="store_true",
                    help="drop the pelvis translation track")
    ap.add_argument("--no-up-convert", action="store_true",
                    help="skip the Z-up -> Y-up root rotation")
    ap.add_argument("--loop", action="store_true",
                    help='set "loop": true (default false)')
    ap.add_argument("--keep-static", action="store_true",
                    help="keep identity-only rotation tracks")
    ap.add_argument("--name", default=None,
                    help="override animation name (default: file stem)")
    args = ap.parse_args()

    channels = {c.strip().lower() for c in args.channels.split(",") if c.strip()}
    bad = channels - {"body", "hands", "face"}
    if bad:
        ap.error(f"unknown channel group(s): {', '.join(sorted(bad))}")
    if not channels:
        ap.error("no channel groups selected")

    common = dict(
        channels=channels,
        out_fps=args.fps,
        root_motion=not args.no_root_motion,
        up_convert=not args.no_up_convert,
        loop=args.loop,
        keep_static=args.keep_static,
    )

    if os.path.isdir(args.input):
        files = sorted(glob.glob(os.path.join(args.input, "**", "*.npz"),
                                 recursive=True))
        if not files:
            ap.error(f"no .npz files under {args.input}")
        out_root = args.output
        print(f"Batch: {len(files)} file(s) from {args.input}")
        ok = 0
        for fp in files:
            rel = os.path.relpath(fp, args.input)
            stem = os.path.splitext(rel)[0]
            if out_root:
                out_path = os.path.join(out_root, stem + ".json")
            else:
                out_path = os.path.splitext(fp)[0] + ".json"
            nm = args.name or os.path.splitext(os.path.basename(fp))[0]
            try:
                convert_file(fp, out_path, name=nm, **common)
                ok += 1
            except Exception as e:
                print(f"  SKIP {rel}: {e}", file=sys.stderr)
        print(f"Done: {ok}/{len(files)} converted.")
    else:
        if not os.path.isfile(args.input):
            ap.error(f"input not found: {args.input}")
        out_path = resolve_single_output(args.input, args.output)
        nm = args.name or os.path.splitext(os.path.basename(args.input))[0]
        convert_file(args.input, out_path, name=nm, **common)


if __name__ == "__main__":
    main()
