# Animation JSON Module

The engine parses a JSON file into an internal `Animation` IR (Intermediate Representation), which is then sampled per frame and applied to a mesh's morph weights and joint matrices.

---

## 1. Files

| Path | Purpose |
|------|---------|
| `src/animation.h` | IR types: `Animation`, `Track`, `Keyframe`, enums. |
| `src/animation_json.h` | JSON adapter interface + loader entry point. |
| `src/animation_json.cpp` | Default parser implementation. |
| `src/application.cpp` (around lines 117–127) | Where the animation is loaded and attached to the character mesh. |

Loading entry point:

```cpp
Animation anim = load_animation_json(
    RESOURCES_PATH "animations/<your_file>.json",
    *skin, *morphs);
character0->set_animation(std::make_unique<Animation>(std::move(anim)));
```

The mesh advances every frame via `mesh->advance_animation(m_time.delta)` in the
main tick loop.

---

## 2. Top-level JSON schema

```json
{
  "name":     "demo",
  "duration": 3.0,
  "fps":      30.0,
  "loop":     true,
  "tracks":   [ ... ]
}
```

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `name` | string | `"unnamed"` | Display/debug only. |
| `duration` | float (seconds) | `0.0` | Total length. Sampling wraps at this time when `loop` is true. |
| `fps` | float | `30.0` | Reference value only — sampling is **time-based**, not frame-based. |
| `loop` | bool | `true` | If false, the pose holds at the last keyframe after `duration`. |
| `tracks` | array | `[]` | List of `Track` objects (see §3). |

All fields are optional except `tracks`.

---

## 3. Tracks

Each track animates **one channel** of **one target**.

```json
{
  "target": "joint:right_shoulder/rotation",
  "interp": "linear",
  "keys":   [ [0.0, [0,0,0,1]], [1.5, [0,0.2,0,0.98]] ]
}
```

### 3.1 Target naming (see `animation_json.cpp:40–80`)

| Prefix form | Mapped channel | Key value type |
|-------------|----------------|----------------|
| `morph:<name>` | `MORPH_WEIGHT` | scalar |
| `joint:<name>/translation` | `JOINT_TRANSLATION` | vec3 |
| `joint:<name>/rotation` | `JOINT_ROTATION` | quat (x,y,z,w) |
| `joint:<name>/scale` | `JOINT_SCALE` | vec3 |
| `node:<name>/translation` | `NODE_TRANSLATION` | vec3 |
| `node:<name>/rotation` | `NODE_ROTATION` | quat (x,y,z,w) |
| `node:<name>/scale` | `NODE_SCALE` | vec3 |

- `<name>` is the joint, node, or morph-target name as it appears in the GLB.
- For Alex, joint names and morph target names (`Exp_000` … `Exp_099`) are listed in
  `resources/alex.glb.params.txt`.
- A track whose prefix doesn't match any known form is **silently skipped**.

### 3.2 Interpolation

`"interp"` may be one of:

| Value | Meaning |
|-------|---------|
| `"step"` | Hold the previous key's value until the next key. |
| `"linear"` *(default)* | Linear interpolation between keys. Quaternions are not slerp'd here — see §6.2. |
| `"cubicspline"` | Cubic-Hermite spline interpolation. |

If omitted, `"linear"` is used.

### 3.3 Keyframe format

Each key is a 2-element array: `[time_seconds, value]`.

| Channel kind | Value shape |
|--------------|-------------|
| Scalar (morph) | `0.7` |
| Vec3 (translate/scale) | `[x, y, z]` |
| Quat (rotation) | `[x, y, z, w]` |

Keys **must be sorted ascending by time**. The loader does not re-sort them.

---

## 4. Minimal examples

### 4.1 Morph-only animation

```json
{
  "name": "blink",
  "duration": 1.0,
  "fps": 30,
  "loop": true,
  "tracks": [
    {
      "target": "morph:Exp_000",
      "interp": "linear",
      "keys": [ [0.0, 0.0], [0.5, 1.0], [1.0, 0.0] ]
    }
  ]
}
```

See `resources/animations/test_morph.json` for a working file.

### 4.2 Single-joint rotation

```json
{
  "target": "joint:right_shoulder/rotation",
  "interp": "linear",
  "keys": [
    [0.0, [0.000, 0.0, 0.0, 1.000]],
    [1.0, [0.259, 0.0, 0.0, 0.966]]
  ]
}
```

See `resources/animations/right_shoulder_x.json` or `resources/animations/right_elbow_y.json` for a joint example.
See `resources/animations/test_anim.json` for a combined joint + morph animation example.

---

## 5. Quaternion encoding

A rotation of angle `θ` around a **unit axis** `(ax, ay, az)` is encoded as:

```
[ ax·sin(θ/2), ay·sin(θ/2), az·sin(θ/2), cos(θ/2) ]
```

Quaternions in the JSON are stored in `[x, y, z, w]` order.

Useful cheat sheet:

| Angle | sin(θ/2) | cos(θ/2) |
|-------|----------|----------|
| 0°    | 0.000    | 1.000    |
| 30°   | 0.259    | 0.966    |
| 45°   | 0.383    | 0.924    |
| 60°   | 0.500    | 0.866    |
| 90°   | 0.707    | 0.707    |