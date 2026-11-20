#!/usr/bin/env python3
"""
face_anim_author.py -- hand-author facial animation with readable controls and
compile it to the engine's animation JSON (see ANIMATION.md).

The characters' faces are driven by 100 SMPL-X expression PCA components
(`morph:Exp_000` .. `morph:Exp_099`) plus the `jaw`, `left_eye` and `right_eye`
joints. PCA components have no meaning on their own, so keying them by hand is
impractical. This tool adds a thin authoring layer on top:

  * **Semantic controls** (`browRaise`, `smile`, `funnel`, `blinkL`, ...) are
    defined as target displacements of face regions (brows, lids, lip corners,
    lips, cheeks, nose) in millimetres. Each control is solved ONCE into a
    100-D expression vector by ridge-regularised least squares against the
    GLB's own morph-target deltas (the rest of the face is asked to stay put).
  * A **keyframe sheet** (JSON) keys those controls plus `jaw` / eye angles in
    degrees over time. The sheet is the source of truth for a clip.
  * The compiler interpolates every channel (monotone cubic -- no overshoot),
    samples at `fps`, sums the control vectors into Exp weights and writes
    dense `linear` tracks (dense because the engine lerps quaternions without
    slerp, and so the cubic easing survives).

The expression basis is identical across the character GLBs (maria, alex,
nadia, javi -- verified bit-identical), so a compiled animation plays on any of
them. Regions are located from the skeleton (eye joints) and the mouth opening
(mesh boundary loop), not hard-coded coordinates.

Usage:
    python tools/face_anim_author.py SHEET.json [OUTPUT.json] [--glb GLB]
    python tools/face_anim_author.py --list-controls [--glb GLB]

    SHEET   keyframe sheet (see tools/face_keys/*.json)
    OUTPUT  defaults to resources/animations/mocap/<sheet name>.json

Sheet format:
    {
      "name": "surprise_1",
      "fps": 30, "loop": false, "duration": 7.0,      # duration optional
      "source": {...},                                 # free-form notes
      "keys": [
        {"t": 0.0, "browRaise": 0.0, "jaw": 0},
        {"t": 0.5, "browRaise": 1.0, "funnel": 0.6, "jaw": 9, "eyesPitch": 3},
        ...
      ]
    }
  Every channel is interpolated independently through the keys that mention
  it; a channel never mentioned stays 0.

  A sheet can instead chain other sheets (paths relative to itself):
    {"name": "diverse_set", "fps": 30, "loop": false, "gap": 0.6,
     "sequence": ["air_kiss.json", "anger.json", ...]}
  Each clip is sampled on its own; between clips every channel eases out to
  neutral and back in over `gap` seconds.

  Channels:
    <control>   weight, nominally 0..1 (may exceed 1 or go negative)
    jaw         jaw opening, degrees (+ = open)
    jawSide     jaw lateral, degrees (+ = chin toward the character's left)
    eyesYaw     gaze, degrees (+ = toward the character's left), both eyes
    eyesPitch   gaze, degrees (+ = up), both eyes
    eyeYawL / eyeYawR / eyePitchL / eyePitchR   per-eye offsets added on top

Only numpy is required.
"""

import argparse
import collections
import json
import math
import os
import struct
import sys

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_GLB = os.path.join(REPO, "resources", "models", "maria", "maria.glb")
OUT_DIR = os.path.join(REPO, "resources", "animations", "mocap")

# --------------------------------------------------------------------------
# GLB reading (mesh 0 = the face/body, which carries the Exp_* morph targets)
# --------------------------------------------------------------------------
_CT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
       5125: np.uint32, 5126: np.float32}
_NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def _read_glb(path):
    b = open(path, "rb").read()
    if b[:4] != b"glTF":
        sys.exit(f"not a GLB: {path}")
    ln = struct.unpack("<I", b[12:16])[0]
    j = json.loads(b[20:20 + ln])
    off = 20 + ln
    bl = struct.unpack("<I", b[off:off + 4])[0]
    binc = b[off + 8:off + 8 + bl]

    def acc(i):
        a = j["accessors"][i]
        n, dt, cnt = _NC[a["type"]], _CT[a["componentType"]], a["count"]
        out = np.zeros((cnt, n), np.float64)
        if "bufferView" in a:
            bv = j["bufferViews"][a["bufferView"]]
            o = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
            isz = np.dtype(dt).itemsize * n
            st = bv.get("byteStride", 0)
            if st and st != isz:
                raw = np.frombuffer(binc, np.uint8, cnt * st, o).reshape(cnt, st)
                out = raw[:, :isz].copy().view(dt).reshape(cnt, n).astype(np.float64)
            else:
                out = np.frombuffer(binc, dt, cnt * n, o).reshape(cnt, n).astype(np.float64)
        if "sparse" in a:  # Blender writes morph deltas as sparse accessors
            s = a["sparse"]
            c = s["count"]
            iv = j["bufferViews"][s["indices"]["bufferView"]]
            vv = j["bufferViews"][s["values"]["bufferView"]]
            idx = np.frombuffer(binc, _CT[s["indices"]["componentType"]], c,
                                iv.get("byteOffset", 0) + s["indices"].get("byteOffset", 0))
            val = np.frombuffer(binc, dt, c * n,
                                vv.get("byteOffset", 0) + s["values"].get("byteOffset", 0))
            out = out.copy()
            out[idx.astype(int)] = val.reshape(c, n)
        return out

    return j, acc


def load_face(glb):
    j, acc = _read_glb(glb)
    prim = j["meshes"][0]["primitives"][0]
    names = j["meshes"][0].get("extras", {}).get("targetNames") or []
    P = acc(prim["attributes"]["POSITION"])
    F = acc(prim["indices"]).astype(np.int64).reshape(-1, 3)
    D = np.stack([acc(t["POSITION"]) for t in prim["targets"]])
    J = acc(prim["attributes"]["JOINTS_0"]).astype(int)
    W = acc(prim["attributes"]["WEIGHTS_0"])
    skin = j["skins"][0]
    jnames = [j["nodes"][i]["name"] for i in skin["joints"]]
    ibm = acc(skin["inverseBindMatrices"]).reshape(-1, 4, 4)
    jpos = {n: np.linalg.inv(ibm[k].T)[:3, 3] for k, n in enumerate(jnames)}
    for n in ("jaw", "left_eye", "right_eye"):
        if n not in jnames:
            sys.exit(f"GLB skin has no '{n}' joint")
        r = j["nodes"][skin["joints"][jnames.index(n)]].get("rotation", [0, 0, 0, 1])
        if abs(abs(r[3]) - 1.0) > 1e-3:
            print(f"warning: joint '{n}' has a non-identity rest rotation {r}; "
                  f"rotation tracks replace it", file=sys.stderr)
    return dict(P=P, F=F, D=D, J=J, W=W, jnames=jnames, jpos=jpos,
                morph_names=names or [f"Exp_{i:03d}" for i in range(len(D))])


# --------------------------------------------------------------------------
# Face regions
# --------------------------------------------------------------------------
def _mouth_loop(P, F, eye_y):
    """Centre, half-width and depth of the mouth opening (a mesh boundary loop)."""
    key = np.round(P * 1e5).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    Fw = inv.ravel()[F]
    cnt = collections.Counter()
    for a, b in ((0, 1), (1, 2), (2, 0)):
        for e in np.sort(np.stack([Fw[:, a], Fw[:, b]], 1), 1):
            cnt[(int(e[0]), int(e[1]))] += 1
    adj = collections.defaultdict(list)
    for (a, b), c in cnt.items():
        if c == 1:
            adj[a].append(b)
            adj[b].append(a)
    Pw = P[first]
    seen, best = set(), None
    for s in adj:
        if s in seen:
            continue
        comp, st = [], [s]
        while st:
            v = st.pop()
            if v in seen:
                continue
            seen.add(v)
            comp.append(v)
            st += adj[v]
        c = Pw[comp]
        w = c[:, 0].max() - c[:, 0].min()
        ctr = c.mean(0)
        # mouth: 3-8 cm wide, centred on x=0, 4-11 cm below the eyes, in front
        if 0.03 < w < 0.08 and abs(ctr[0]) < 0.01 and 0.04 < eye_y - ctr[1] < 0.11:
            if best is None or len(comp) > best[0]:
                best = (len(comp), ctr, w / 2)
    if best is None:
        sys.exit("could not find the mouth opening in the face mesh")
    return best[1], best[2]


def build_regions(m):
    P, J, W, jn = m["P"], m["J"], m["W"], m["jnames"]

    def jw(n):
        return ((J == jn.index(n)) * W).sum(1)

    eL, eR = m["jpos"]["left_eye"], m["jpos"]["right_eye"]
    eye_y = (eL[1] + eR[1]) / 2
    mc, mhw = _mouth_loop(m["P"], m["F"], eye_y)
    eyeball = (jw("left_eye") + jw("right_eye")) > 0.5
    face = ((P[:, 1] > mc[1] - 0.075) & (P[:, 2] > mc[2] - 0.08)
            & (np.abs(P[:, 0]) < 0.085) & (P[:, 1] < eye_y + 0.07) & ~eyeball)
    R = {}
    for s, e in (("L", eL), ("R", eR)):
        sx = 1.0 if s == "L" else -1.0
        d = np.linalg.norm(P - e, axis=1)
        near_x = np.abs(P[:, 0] - e[0]) < 0.009
        R["lidU" + s] = face & (d < 0.019) & (P[:, 1] > e[1] + 0.004) & (P[:, 2] > e[2] + 0.004) & near_x
        R["lidL" + s] = face & (d < 0.019) & (P[:, 1] < e[1] - 0.004) & (P[:, 2] > e[2]) & near_x
        brow = face & (P[:, 1] > e[1] + 0.012) & (P[:, 1] < e[1] + 0.030) & (P[:, 2] > e[2])
        R["browIn" + s] = brow & (sx * P[:, 0] > 0.006) & (sx * P[:, 0] < 0.022)
        R["browOut" + s] = brow & (sx * P[:, 0] > 0.034) & (sx * P[:, 0] < 0.052)
        R["cheek" + s] = (face & (P[:, 1] > e[1] - 0.055) & (P[:, 1] < e[1] - 0.025)
                          & (sx * P[:, 0] > 0.030) & (sx * P[:, 0] < 0.055) & (P[:, 2] > mc[2] - 0.04))
        corner = np.array([sx * (mhw - 0.0005), mc[1], mc[2] - 0.004])
        R["corner" + s] = face & (np.linalg.norm(P - corner, axis=1) < 0.007)
    R["lipU"] = face & (np.abs(P[:, 0]) < 0.012) & (P[:, 1] > mc[1]) & (P[:, 1] < mc[1] + 0.013) & (P[:, 2] > mc[2] - 0.003)
    R["lipL"] = face & (np.abs(P[:, 0]) < 0.012) & (P[:, 1] < mc[1]) & (P[:, 1] > mc[1] - 0.016) & (P[:, 2] > mc[2] - 0.006)
    R["nose"] = (face & (np.abs(P[:, 0]) < 0.022) & (P[:, 1] > mc[1] + 0.015)
                 & (P[:, 1] < mc[1] + 0.04) & (P[:, 2] > mc[2] + 0.007))
    for k, v in R.items():
        if v.sum() < 4:
            sys.exit(f"region '{k}' found only {v.sum()} vertices")
    return R, face


# --------------------------------------------------------------------------
# Semantic controls: region -> displacement in mm (out, up, fwd).
# "out" points away from the face midline, so one entry serves both sides.
# Side-less region names ("corner", "brow"...) expand to L and R.
# --------------------------------------------------------------------------
CONTROLS = {
    # brows
    "browRaise":      {"browIn": (0, 6, 0), "browOut": (0, 5, 0)},
    "browRaiseInner": {"browIn": (0, 6, 0), "browOut": (0, 0, 0)},
    "browLower":      {"browIn": (-2, -4, 0), "browOut": (0, -2, 0)},
    # eyes
    "eyeWide":        {"lidU": (0, 2.5, 0)},
    "blink":          {"lidU": (0, -9, 1), "lidL": (0, 1.5, 0)},
    # narrows the eye from both lids (a lower-lid-only squint barely reads)
    "squint":         {"lidL": (0, 3, 0), "lidU": (0, -2.5, 0), "cheek": (0, 2.5, 0.5)},
    # mouth
    "smile":          {"corner": (5, 6, -3), "cheek": (0, 3, 1), "lipU": (0, 1, 0)},
    "frown":          {"corner": (1, -5, 0)},
    "stretch":        {"corner": (6, -1, -2)},
    "funnel":         {"corner": (-7, 0, 4), "lipU": (0, 1, 4), "lipL": (0, -1, 4)},
    "pucker":         {"corner": (-8, 0, 5), "lipU": (0, -1, 5), "lipL": (0, 1, 5)},
    "upperLipRaise":  {"lipU": (0, 4, 1), "nose": (0, 1, 0)},
    "lowerLipDown":   {"lipL": (0, -4, 1)},
    "lipsPress":      {"lipU": (0, -1.5, 0), "lipL": (0, 1.5, 0)},
    "cheekPuff":      {"cheek": (4, 0, 3)},
    "sneer":          {"nose": (0, 2, 0), "lipU": (0, 2, 0), "cheek": (0, 1, 0)},
}
# Controls that also get one-sided variants (<name>L / <name>R).
SIDED = ("browRaise", "browLower", "blink", "squint", "smile", "frown", "eyeWide")

RIDGE = 0.02        # coefficient penalty (relative to target-region weight)
REST_WEIGHT = 0.3   # weight of "stay put" on face vertices far from the targets
BUFFER_MM = 12.0    # unconstrained margin around the targeted regions


def _expand(spec, sides):
    """Region -> (target vec, out-sign). A one-sided control also pins the
    mirrored regions to zero at full weight: the PCA basis is mostly
    symmetric, so a weak 'stay put' alone lets a wink close both eyes."""
    out = {}
    for reg, vec in spec.items():
        if reg in ("lipU", "lipL", "nose"):
            out[reg] = (vec, 0.0)
        else:
            for s in "LR":
                sx = 1.0 if s == "L" else -1.0
                out[reg + s] = (vec if s in sides else (0, 0, 0), sx)
    return out


def solve_controls(m, R, face):
    P, D = m["P"], m["D"] * 1000.0          # deltas in mm per unit weight
    fidx = np.where(face)[0]
    A = D[:, fidx, :].transpose(1, 2, 0)    # (Nf, 3, K)
    K = D.shape[0]
    result = {}
    defs = {}
    for name, spec in CONTROLS.items():
        defs[name] = _expand(spec, "LR")
        if name in SIDED:
            defs[name + "L"] = _expand(spec, "L")
            defs[name + "R"] = _expand(spec, "R")
    for name, regs in defs.items():
        rows, rhs, wts = [], [], []
        targeted = np.zeros(len(P), bool)
        for reg, (vec, sx) in regs.items():
            mask = R[reg]
            targeted |= mask
            t = np.array([vec[0] * sx, vec[1], vec[2]], float)
            if reg in ("lipU", "lipL", "nose"):
                t[0] = 0.0
            sel = np.where(mask[fidx])[0]
            w = 1.0 / math.sqrt(len(sel))   # each region counts the same
            rows.append(A[sel].reshape(-1, K))
            rhs.append(np.tile(t, len(sel)))
            wts.append(np.full(len(sel) * 3, w))
        # rest of the face: stay put, except a buffer around the targets
        tp = P[targeted]
        d = np.full(len(fidx), np.inf)
        for chunk in np.array_split(np.arange(len(fidx)), 8):
            dd = np.linalg.norm(P[fidx[chunk]][:, None, :] - tp[None], axis=2).min(1)
            d[chunk] = dd
        rest = np.where(d * 1000.0 > BUFFER_MM)[0]
        rows.append(A[rest].reshape(-1, K))
        rhs.append(np.zeros(len(rest) * 3))
        wts.append(np.full(len(rest) * 3, REST_WEIGHT / math.sqrt(len(rest)) * math.sqrt(len(regs))))
        Am = np.concatenate(rows)
        b = np.concatenate(rhs)
        w = np.concatenate(wts)
        Aw = Am * w[:, None]
        bw = b * w
        lam = RIDGE * len(regs)
        e = np.linalg.solve(Aw.T @ Aw + lam * np.eye(K), Aw.T @ bw)
        # report achieved vs target per region
        rep = []
        for reg, (vec, sx) in regs.items():
            got = (D[:, R[reg], :].mean(1).T @ e)
            got[0] *= (sx if sx else 1.0)
            rep.append(f"{reg}=({got[0]:+.1f},{got[1]:+.1f},{got[2]:+.1f})")
        result[name] = (e, rep)
    return result


# --------------------------------------------------------------------------
# Interpolation (monotone cubic / PCHIP, Fritsch-Carlson)
# --------------------------------------------------------------------------
def pchip(xk, yk, x):
    xk, yk = np.asarray(xk, float), np.asarray(yk, float)
    if len(xk) == 1:
        return np.full_like(x, yk[0])
    h = np.diff(xk)
    dlt = np.diff(yk) / h
    n = len(xk)
    d = np.zeros(n)
    if n == 2:
        d[:] = 0.0                      # ease in / ease out
    else:
        for i in range(1, n - 1):
            if dlt[i - 1] * dlt[i] > 0:
                w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
                d[i] = (w1 + w2) / (w1 / dlt[i - 1] + w2 / dlt[i])
        d[0] = d[-1] = 0.0              # start/end at rest velocity
    xc = np.clip(x, xk[0], xk[-1])
    i = np.clip(np.searchsorted(xk, xc, side="right") - 1, 0, n - 2)
    t = (xc - xk[i]) / h[i]
    h00, h10 = 2 * t**3 - 3 * t**2 + 1, t**3 - 2 * t**2 + t
    h01, h11 = -2 * t**3 + 3 * t**2, t**3 - t**2
    return h00 * yk[i] + h10 * h[i] * d[i] + h01 * yk[i + 1] + h11 * h[i] * d[i + 1]


# --------------------------------------------------------------------------
# Joint quaternions. Jaw/eye local frames (identical in all character GLBs):
# +X = character's left, +Y = forward, +Z = down. Rotation about +X turns
# forward toward down (jaw opens / gaze drops); about +Z turns forward toward
# the character's right.
# --------------------------------------------------------------------------
def qaxis(axis, deg):
    a = math.radians(deg) / 2
    v = np.zeros(4)
    v[axis] = math.sin(a)
    v[3] = math.cos(a)
    return v


def qmul(a, b):
    x1, y1, z1, w1 = a
    x2, y2, z2, w2 = b
    return np.array([w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
                     w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
                     w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
                     w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2])


def num(x):
    s = f"{x:.5f}".rstrip("0").rstrip(".")
    return 0 if s in ("", "-0") else float(s)


JOINT_CHANNELS = {"jaw", "jawSide", "eyesYaw", "eyesPitch",
                  "eyeYawL", "eyeYawR", "eyePitchL", "eyePitchR"}


def sample_sheet(sheet, controls, fps):
    """Interpolate every channel of a keyframe sheet -> (n, {channel: (n,)})."""
    keys = sorted(sheet["keys"], key=lambda k: k["t"])
    dur = float(sheet.get("duration", keys[-1]["t"]))
    n = int(round(dur * fps)) + 1
    ts = np.arange(n) / fps
    chans = collections.OrderedDict()
    for k in keys:
        for c, v in k.items():
            if c == "t" or c.startswith("_"):
                continue
            if c not in controls and c not in JOINT_CHANNELS:
                sys.exit(f"unknown channel '{c}' at t={k['t']} in "
                         f"'{sheet.get('name')}' (see --list-controls)")
            chans.setdefault(c, []).append((float(k["t"]), float(v)))
    curve = {}
    for c, kv in chans.items():
        if kv[0][0] > 0:
            kv = [(0.0, 0.0)] + kv
        curve[c] = pchip([a for a, _ in kv], [b for _, b in kv], ts)
    return n, curve


def sample_sequence(sheet, sheet_dir, controls, fps):
    """Chain several clip sheets. Between clips every channel eases from the
    outgoing clip's last value to neutral and on to the incoming clip's first
    value (smoothstep over gap/2 each), so held values never leak across."""
    gap = float(sheet.get("gap", 0.6))
    half = max(1, int(round(gap * fps / 2)))
    parts = []
    for ref in sheet["sequence"]:
        sub = json.load(open(os.path.join(sheet_dir, ref)))
        parts.append(sample_sheet(sub, controls, fps))
    names = list(dict.fromkeys(c for _, cv in parts for c in cv))
    s = np.arange(1, half + 1) / (half + 1)
    ease = s * s * (3 - 2 * s)
    out = {c: [] for c in names}
    for i, (n, cv) in enumerate(parts):
        for c in names:
            seg = cv.get(c, np.zeros(n))
            if i > 0:                       # ease in from neutral
                out[c].append(np.zeros(1))
                out[c].append(ease * seg[0])
            out[c].append(seg)
            if i < len(parts) - 1:          # ease out to neutral
                out[c].append((1 - ease) * seg[-1])
    curve = {c: np.concatenate(v) for c, v in out.items()}
    return len(next(iter(curve.values()))), curve


def compile_sheet(sheet, controls, morph_names, sheet_dir="."):
    fps = float(sheet.get("fps", 30))
    if "sequence" in sheet:
        n, curve = sample_sequence(sheet, sheet_dir, controls, fps)
    else:
        n, curve = sample_sheet(sheet, controls, fps)
    ts = np.arange(n) / fps
    dur = (n - 1) / fps
    zero = np.zeros(n)
    g = lambda c: curve.get(c, zero)

    K = len(morph_names)
    E = np.zeros((n, K))
    for c in curve:
        if c in controls:
            E += np.outer(curve[c], controls[c][0])

    tracks = []
    for k in range(K):
        if np.abs(E[:, k]).max() < 1e-3:
            continue
        tracks.append({"target": f"morph:{morph_names[k]}", "interp": "linear",
                       "keys": [[num(t), num(v)] for t, v in zip(ts, E[:, k])]})

    def qtrack(name, qs):
        return {"target": f"joint:{name}/rotation", "interp": "linear",
                "keys": [[num(t), [num(c) for c in q]] for t, q in zip(ts, qs)]}

    if "jaw" in curve or "jawSide" in curve:
        qs = [qmul(qaxis(0, a), qaxis(2, -s)) for a, s in zip(g("jaw"), g("jawSide"))]
        tracks.append(qtrack("jaw", qs))
    if any(c in curve for c in ("eyesYaw", "eyesPitch", "eyeYawL", "eyeYawR",
                                "eyePitchL", "eyePitchR")):
        for side, nm in (("L", "left_eye"), ("R", "right_eye")):
            yaw = g("eyesYaw") + g("eyeYaw" + side)
            pit = g("eyesPitch") + g("eyePitch" + side)
            qs = [qmul(qaxis(2, -y), qaxis(0, -p)) for y, p in zip(yaw, pit)]
            tracks.append(qtrack(nm, qs))

    return {"name": sheet.get("name", "unnamed"), "duration": num(dur),
            "fps": fps, "loop": bool(sheet.get("loop", False)), "tracks": tracks}


def write_anim(anim, path):
    """One key per line, like the other files in resources/animations."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    head = {k: v for k, v in anim.items() if k != "tracks"}
    lines = ["{"]
    for k, v in head.items():
        lines.append(f'  "{k}": {json.dumps(v)},')
    lines.append('  "tracks": [')
    for ti, t in enumerate(anim["tracks"]):
        lines.append("    {")
        lines.append(f'      "target": "{t["target"]}",')
        lines.append(f'      "interp": "{t["interp"]}",')
        lines.append('      "keys": [')
        ks = [f"        {json.dumps(k, separators=(', ', ': '))}" for k in t["keys"]]
        lines.append(",\n".join(ks))
        lines.append("      ]")
        lines.append("    }" + ("," if ti < len(anim["tracks"]) - 1 else ""))
    lines.append("  ]")
    lines.append("}")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sheet", nargs="?")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--glb", default=DEFAULT_GLB,
                    help="character GLB whose morph targets define the basis")
    ap.add_argument("--list-controls", action="store_true",
                    help="print every control with the displacement it achieves")
    a = ap.parse_args()
    if not a.sheet and not a.list_controls:
        ap.error("SHEET is required (or --list-controls)")

    m = load_face(a.glb)
    R, face = build_regions(m)
    controls = solve_controls(m, R, face)

    if a.list_controls:
        print("control            |e|   max|e|  achieved mean displacement (out, up, fwd) mm")
        for name, (e, rep) in controls.items():
            print(f"{name:16s} {np.linalg.norm(e):6.2f} {np.abs(e).max():6.2f}  " + " ".join(rep))
        print("\njoint channels (degrees): " + ", ".join(sorted(JOINT_CHANNELS)))
        if not a.sheet:
            return

    sheet = json.load(open(a.sheet))
    anim = compile_sheet(sheet, controls, m["morph_names"],
                         os.path.dirname(os.path.abspath(a.sheet)))
    out = a.output or os.path.join(OUT_DIR, f"{anim['name']}.json")
    write_anim(anim, out)
    print(f"wrote {out}: {len(anim['tracks'])} tracks, {anim['duration']} s @ {anim['fps']} fps")


if __name__ == "__main__":
    main()
