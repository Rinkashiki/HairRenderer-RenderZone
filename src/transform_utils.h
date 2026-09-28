#pragma once
// Matrix <-> Object3D transform helpers for the editor tools (gizmo, undo).
//
// Object3D::get_model_matrix() composes  T * Rx(x) * Ry(y) * Rz(z) * S  with
// Euler angles stored in degrees. ImGuizmo's DecomposeMatrixToComponents assumes
// the opposite order (Rz * Ry * Rx), so feeding its angles back into an Object3D
// silently changes any multi-axis rotation — never use it for write-back.

#include <cmath>
#include <glm/glm.hpp>

namespace transform_utils {

struct TRS {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // degrees, engine order (Rx * Ry * Rz)
    glm::vec3 scale{1.0f};
};

namespace detail {
// Shift `a` by whole turns so it lands as close as possible to `ref` (radians).
inline float unwrap_near(float a, float ref) {
    const float twoPi = 6.28318530718f;
    return a + twoPi * std::round((ref - a) / twoPi);
}
inline float angle_cost(const glm::vec3& a, const glm::vec3& ref) {
    const glm::vec3 d = a - ref;
    return glm::dot(d, d);
}
} // namespace detail

// Decompose `m` (= T * Rx * Ry * Rz * S, no shear) into engine-order TRS.
// `prevRotationDeg` picks, among the equivalent Euler triples, the one closest to
// the object's current angles, so values don't flip (e.g. 0/180/0 <-> 180/0/180)
// and stay continuous across a drag. Handles the ±90° Y gimbal case by keeping
// the previous Z.
inline TRS decompose(const glm::mat4& m, const glm::vec3& prevRotationDeg) {
    TRS out;
    out.position = glm::vec3(m[3]);

    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    out.scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    // A mirrored basis can't be expressed as a rotation: fold the reflection
    // into X scale (the same convention a negative scale.x recomposes to).
    if (glm::dot(glm::cross(c0, c1), c2) < 0.0f)
        out.scale.x = -out.scale.x;
    const float eps = 1e-8f;
    c0 /= (std::abs(out.scale.x) > eps ? out.scale.x : 1.0f);
    c1 /= (out.scale.y > eps ? out.scale.y : 1.0f);
    c2 /= (out.scale.z > eps ? out.scale.z : 1.0f);

    // R(row, col) for R = Rx(a) * Ry(b) * Rz(c):
    //   R(0,0)= cb*cc   R(0,1)=-cb*sc   R(0,2)= sb
    //   R(1,2)=-sa*cb   R(2,2)= ca*cb
    // glm is column-major: R(row, col) == c<col>[row].
    const glm::vec3 prev = glm::radians(prevRotationDeg);
    const float     sb   = glm::clamp(c2.x, -1.0f, 1.0f);
    const float     cb   = std::sqrt(c0.x * c0.x + c1.x * c1.x);

    glm::vec3 best;
    if (cb < 1e-5f) {
        // Gimbal lock (b = ±90°): only a+c (or c-a) is determined — keep c.
        const float b   = sb > 0.0f ? 1.57079632679f : -1.57079632679f;
        const float phi = std::atan2(c0.y, c1.y); // R(1,0), R(1,1)
        const float c   = prev.z;
        const float a   = sb > 0.0f ? phi - c : c - phi;
        best            = {detail::unwrap_near(a, prev.x), detail::unwrap_near(b, prev.y), c};
    } else {
        const float a = std::atan2(-c2.y, c2.z);
        const float b = std::atan2(sb, cb);
        const float c = std::atan2(-c1.x, c0.x);
        // The other triple describing the same rotation: (a+π, π-b, c+π).
        const float     pi = 3.14159265359f;
        const glm::vec3 s1{detail::unwrap_near(a, prev.x), detail::unwrap_near(b, prev.y), detail::unwrap_near(c, prev.z)};
        const glm::vec3 s2{detail::unwrap_near(a + pi, prev.x), detail::unwrap_near(pi - b, prev.y),
                           detail::unwrap_near(c + pi, prev.z)};
        best = detail::angle_cost(s1, prev) <= detail::angle_cost(s2, prev) ? s1 : s2;
    }
    out.rotation = glm::degrees(best);
    return out;
}

} // namespace transform_utils
