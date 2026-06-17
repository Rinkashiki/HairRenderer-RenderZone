#include "hair_binding.h"

#include <engine/core/geometries/geometry.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <fstream>
#include <iostream>
#include <cstring>
#include <limits>
#include <cmath>
#include <algorithm>

using namespace VKFW;

namespace hair_binding {

namespace {

// Closest point on triangle (A,B,C) to P, returning the point and its
// barycentric coords. Ericson, Real-Time Collision Detection §5.1.5.
Vec3 closest_point_on_triangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, Vec3& baryOut) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) { baryOut = {1, 0, 0}; return a; }

    const Vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) { baryOut = {0, 1, 0}; return b; }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        baryOut = {1 - v, v, 0};
        return a + v * ab;
    }

    const Vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) { baryOut = {0, 0, 1}; return c; }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        baryOut = {1 - w, 0, w};
        return a + w * ac;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        baryOut = {0, 1 - w, w};
        return b + w * (c - b);
    }

    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    baryOut = {1 - v - w, v, w};
    return a + ab * v + ac * w;
}

// Orthonormal root frame for a triangle: tangent along edge0, normal from the
// face, bitangent = n × t. Returns false for a degenerate triangle.
bool triangle_frame(const Vec3& p0, const Vec3& p1, const Vec3& p2, Vec3& t, Vec3& b, Vec3& n) {
    const Vec3 e1 = p1 - p0, e2 = p2 - p0;
    Vec3 nn = glm::cross(e1, e2);
    const float nl = glm::length(nn);
    const float tl = glm::length(e1);
    if (nl < 1e-12f || tl < 1e-12f) return false;
    n = nn / nl;
    t = e1 / tl;
    b = glm::cross(n, t); // unit: n ⟂ t
    return true;
}

} // namespace

HairBinder::HairBinder(Core::Mesh* hairMesh, Core::Mesh* headMesh)
    : m_hair(hairMesh)
    , m_head(headMesh) {
}

void HairBinder::anchor_to_head() {
    // Make the hair's model matrix equal the head's so reconstructed head-local
    // vertices land in the world correctly. Skeletal + morph motion comes from
    // the deformed head surface, not from a joint attachment.
    m_hair->set_parent(m_head);
    m_hair->set_position(Vec3(0.0f));
    m_hair->set_rotation(Vec3(0.0f));
    m_hair->set_scale(1.0f);
}

bool HairBinder::bind(float normalOffset, bool declip) {
    if (!m_hair || !m_head) return false;
    Core::Geometry* hairGeom = m_hair->get_geometry(0);
    Core::Geometry* headGeom = m_head->get_geometry(0);
    if (!hairGeom || !headGeom) return false;

    const std::vector<uint32_t>& offsets   = hairGeom->get_strand_offsets();
    const auto&                  hairVerts  = hairGeom->get_properties().vertexData;
    const auto&                  headVerts  = headGeom->get_properties().vertexData; // rest pose
    const std::vector<uint32_t>& headIdx    = headGeom->get_properties().vertexIndex;
    if (offsets.size() < 2 || hairVerts.empty() || headVerts.empty() || headIdx.size() < 3) {
        std::cerr << "[hair_binding] bind aborted: missing strand offsets / head triangles\n";
        return false;
    }

    const size_t nStrands = offsets.size() - 1;
    const size_t nVerts   = hairVerts.size();

    // Gross alignment: hair-local → head-local.
    const Mat4 A = glm::inverse(m_head->get_model_matrix()) * m_hair->get_model_matrix();
    const Mat3 Arot(A);

    m_strands.assign(nStrands, {});
    m_localPos.assign(nVerts, Vec3(0.0f));
    m_localTangent.assign(nVerts, Vec3(0.0f));

    // Diagnostics: how far each root had to travel to reach the scalp surface.
    double sumDist = 0.0; float maxDist = 0.0f;
    double sumShift = 0.0; float maxShift = 0.0f;
    size_t boundCount = 0;

    for (size_t s = 0; s < nStrands; ++s) {
        const uint32_t vBegin = offsets[s];
        const uint32_t vEnd   = offsets[s + 1];
        if (vBegin >= vEnd) continue;

        const Vec3 rootH = Vec3(A * Vec4(hairVerts[vBegin].pos, 1.0f));

        // Nearest head triangle to the strand root (brute force; one-time bind).
        float    bestDist2 = std::numeric_limits<float>::max();
        uint32_t bestI0 = 0, bestI1 = 0, bestI2 = 0;
        Vec3     bestBary(1, 0, 0);
        for (size_t f = 0; f + 2 < headIdx.size(); f += 3) {
            const uint32_t i0 = headIdx[f], i1 = headIdx[f + 1], i2 = headIdx[f + 2];
            Vec3 bary;
            const Vec3 cp = closest_point_on_triangle(
                rootH, headVerts[i0].pos, headVerts[i1].pos, headVerts[i2].pos, bary);
            const float d2 = glm::dot(cp - rootH, cp - rootH);
            if (d2 < bestDist2) {
                bestDist2 = d2;
                bestI0 = i0; bestI1 = i1; bestI2 = i2;
                bestBary = bary;
            }
        }

        StrandBind& sb = m_strands[s];
        sb.tri[0] = bestI0; sb.tri[1] = bestI1; sb.tri[2] = bestI2;
        sb.bary[0] = bestBary.x; sb.bary[1] = bestBary.y; sb.bary[2] = bestBary.z;

        // Bind-pose root frame from the rest triangle.
        const Vec3 P0 = headVerts[bestI0].pos, P1 = headVerts[bestI1].pos, P2 = headVerts[bestI2].pos;
        const Vec3 origin = bestBary.x * P0 + bestBary.y * P1 + bestBary.z * P2;
        Vec3 t, b, n;
        if (!triangle_frame(P0, P1, P2, t, b, n)) { t = {1, 0, 0}; b = {0, 0, 1}; n = {0, 1, 0}; }

        // Express every strand vertex in the bind root frame.
        for (uint32_t v = vBegin; v < vEnd; ++v) {
            const Vec3 pH  = Vec3(A * Vec4(hairVerts[v].pos, 1.0f));
            const Vec3 rel = pH - origin;
            m_localPos[v] = {glm::dot(rel, t), glm::dot(rel, b), glm::dot(rel, n)};

            const Vec3 tH = Arot * hairVerts[v].tangent;
            m_localTangent[v] = {glm::dot(tH, t), glm::dot(tH, b), glm::dot(tH, n)};
        }

        // Snap: rigidly shift the whole strand along the normal so the root sits
        // at normalOffset above the surface (no clip, no float).
        const float shift = m_localPos[vBegin].z - normalOffset;
        for (uint32_t v = vBegin; v < vEnd; ++v)
            m_localPos[v].z -= shift;

        // Declip: lift any vertex below the root's scalp plane up onto it. local.z
        // is the signed height above the surface at the root, so clamping to 0
        // keeps strand bodies out of a head that's fatter than the groom.
        if (declip)
            for (uint32_t v = vBegin; v < vEnd; ++v)
                m_localPos[v].z = std::max(m_localPos[v].z, 0.0f);

        const float dist = std::sqrt(bestDist2);
        sumDist += dist; maxDist = std::max(maxDist, dist);
        sumShift += std::fabs(shift); maxShift = std::max(maxShift, std::fabs(shift));
        ++boundCount;
    }

    if (boundCount) {
        std::cout << "[hair_binding] bound '" << m_hair->get_name() << "': " << boundCount
                  << " strands | root->surface dist avg " << (sumDist / boundCount) << " max " << maxDist
                  << " | snap avg " << (sumShift / boundCount) << " max " << maxShift
                  << " (head-local units)\n";
    }

    m_workVerts        = hairVerts; // keep uv / color, overwrite pos / tangent per frame
    m_normalOffset     = normalOffset;
    m_bound            = true;
    m_reconstructedOnce = false;

    anchor_to_head();
    update(); // initial reconstruction at the current (rest) pose

    // Refresh the mesh bounds so camera frustum culling matches the bound
    // positions (the original bounds were for the misaligned source pose).
    hairGeom->update_bounds(m_workVerts);
    m_hair->setup_volume();
    return true;
}

void HairBinder::update() {
    if (!m_bound) return;
    // Static head: reconstruct once (the rest fit) and stop re-uploading.
    if (!m_head->has_animation() && m_reconstructedOnce) return;

    Core::Geometry* hairGeom = m_hair->get_geometry(0);
    Core::Geometry* headGeom = m_head->get_geometry(0);
    if (!hairGeom || !headGeom) return;

    // Prefer the head's deformed surface; fall back to rest pose if undeformed.
    const auto& deformed = headGeom->get_deformed_vertices();
    const auto& headVerts = deformed.empty() ? headGeom->get_properties().vertexData : deformed;
    if (headVerts.empty()) return;

    const std::vector<uint32_t>& offsets = hairGeom->get_strand_offsets();
    if (offsets.size() < 2) return;

    for (size_t s = 0; s < m_strands.size(); ++s) {
        const StrandBind& sb = m_strands[s];
        const Vec3 P0 = headVerts[sb.tri[0]].pos, P1 = headVerts[sb.tri[1]].pos, P2 = headVerts[sb.tri[2]].pos;
        const Vec3 origin = sb.bary[0] * P0 + sb.bary[1] * P1 + sb.bary[2] * P2;
        Vec3 t, b, n;
        if (!triangle_frame(P0, P1, P2, t, b, n)) { t = {1, 0, 0}; b = {0, 0, 1}; n = {0, 1, 0}; }

        for (uint32_t v = offsets[s]; v < offsets[s + 1]; ++v) {
            const Vec3 lp = m_localPos[v];
            m_workVerts[v].pos = origin + t * lp.x + b * lp.y + n * lp.z;
            const Vec3 lt = m_localTangent[v];
            m_workVerts[v].tangent = glm::normalize(t * lt.x + b * lt.y + n * lt.z);
        }
    }

    // Only latch the static-head guard once the GPU buffer is actually written
    // (the VBO may not be uploaded yet on the first frames after load).
    if (hairGeom->upload_vertices(m_workVerts))
        m_reconstructedOnce = true;
}

// ─── sidecar IO ──────────────────────────────────────────────────────────────

namespace {
constexpr char    HBND_MAGIC[4] = {'H', 'B', 'N', 'D'};
constexpr uint32_t HBND_VERSION  = 1;
} // namespace

bool HairBinder::save(const std::string& path) const {
    if (!m_bound) { std::cerr << "[hair_binding] save: not bound\n"; return false; }
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::cerr << "[hair_binding] save: cannot open " << path << "\n"; return false; }

    const uint32_t nStrands = static_cast<uint32_t>(m_strands.size());
    const uint32_t nVerts   = static_cast<uint32_t>(m_localPos.size());
    f.write(HBND_MAGIC, 4);
    f.write(reinterpret_cast<const char*>(&HBND_VERSION), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&nStrands), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&nVerts), sizeof(uint32_t));
    f.write(reinterpret_cast<const char*>(&m_normalOffset), sizeof(float));
    f.write(reinterpret_cast<const char*>(m_strands.data()), nStrands * sizeof(StrandBind));
    f.write(reinterpret_cast<const char*>(m_localPos.data()), nVerts * sizeof(Vec3));
    f.write(reinterpret_cast<const char*>(m_localTangent.data()), nVerts * sizeof(Vec3));
    return static_cast<bool>(f);
}

bool HairBinder::load(const std::string& path) {
    if (!m_hair || !m_head) return false;
    Core::Geometry* hairGeom = m_hair->get_geometry(0);
    if (!hairGeom) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "[hair_binding] load: cannot open " << path << "\n"; return false; }

    char magic[4];
    uint32_t version = 0, nStrands = 0, nVerts = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
    f.read(reinterpret_cast<char*>(&nStrands), sizeof(uint32_t));
    f.read(reinterpret_cast<char*>(&nVerts), sizeof(uint32_t));
    f.read(reinterpret_cast<char*>(&m_normalOffset), sizeof(float));
    if (!f || std::memcmp(magic, HBND_MAGIC, 4) != 0 || version != HBND_VERSION) {
        std::cerr << "[hair_binding] load: bad header in " << path << "\n";
        return false;
    }

    const std::vector<uint32_t>& offsets = hairGeom->get_strand_offsets();
    const auto&                  hairVerts = hairGeom->get_properties().vertexData;
    if (offsets.size() != nStrands + 1 || hairVerts.size() != nVerts) {
        std::cerr << "[hair_binding] load: binding does not match hair geometry "
                  << "(strands " << nStrands << " vs " << (offsets.size() ? offsets.size() - 1 : 0)
                  << ", verts " << nVerts << " vs " << hairVerts.size() << ")\n";
        return false;
    }

    m_strands.resize(nStrands);
    m_localPos.resize(nVerts);
    m_localTangent.resize(nVerts);
    f.read(reinterpret_cast<char*>(m_strands.data()), nStrands * sizeof(StrandBind));
    f.read(reinterpret_cast<char*>(m_localPos.data()), nVerts * sizeof(Vec3));
    f.read(reinterpret_cast<char*>(m_localTangent.data()), nVerts * sizeof(Vec3));
    if (!f) { std::cerr << "[hair_binding] load: truncated " << path << "\n"; return false; }

    m_workVerts        = hairVerts;
    m_bound            = true;
    m_reconstructedOnce = false;
    anchor_to_head();
    update();

    hairGeom->update_bounds(m_workVerts);
    m_hair->setup_volume();
    return true;
}

} // namespace hair_binding
