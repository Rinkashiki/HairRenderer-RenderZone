#include "picking.h"

#include <cmath>
#include <limits>

namespace picking {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

bool effectively_active(Core::Object3D* obj) {
    for (Core::Object3D* o = obj; o; o = o->get_parent())
        if (!o->is_active())
            return false;
    return true;
}

// Slab test; `d` need not be normalized.
bool ray_hits_aabb(const Vec3& o, const Vec3& d, const Vec3& mn, const Vec3& mx, float maxT) {
    float t0 = 0.0f, t1 = maxT;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-12f) {
            if (o[a] < mn[a] || o[a] > mx[a])
                return false;
            continue;
        }
        float inv = 1.0f / d[a];
        float tn  = (mn[a] - o[a]) * inv;
        float tf  = (mx[a] - o[a]) * inv;
        if (tn > tf)
            std::swap(tn, tf);
        t0 = std::max(t0, tn);
        t1 = std::min(t1, tf);
        if (t0 > t1)
            return false;
    }
    return true;
}

// Möller–Trumbore, two-sided. Returns t (in units of |d|) or kInf.
float ray_triangle(const Vec3& o, const Vec3& d, const Vec3& v0, const Vec3& v1, const Vec3& v2) {
    const Vec3  e1  = v1 - v0;
    const Vec3  e2  = v2 - v0;
    const Vec3  p   = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-14f)
        return kInf;
    const float inv = 1.0f / det;
    const Vec3  s   = o - v0;
    const float u   = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f)
        return kInf;
    const Vec3  q = glm::cross(s, e1);
    const float v = glm::dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return kInf;
    const float t = glm::dot(e2, q) * inv;
    return t > 0.0f ? t : kInf;
}

// Distance along the (normalized) ray to the segment's closest approach, if the
// segment passes within `tolPerUnit * t` of the ray (a constant pixel radius).
float ray_segment(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, float tolPerUnit) {
    const Vec3  v     = b - a;
    const Vec3  w0    = o - a;
    const float bb    = glm::dot(d, v);
    const float cc    = glm::dot(v, v);
    const float dd    = glm::dot(d, w0);
    const float ee    = glm::dot(v, w0);
    const float denom = cc - bb * bb; // |d| == 1
    float       s     = denom > 1e-12f ? (ee - bb * dd) / denom : 0.0f;
    s                 = glm::clamp(s, 0.0f, 1.0f);
    const Vec3  q     = a + v * s;
    const float t     = glm::dot(d, q - o);
    if (t <= 0.0f)
        return kInf;
    const Vec3 onRay = o + d * t;
    return glm::length(q - onRay) <= tolPerUnit * t ? t : kInf;
}

const std::vector<Graphics::Vertex>* current_vertices(Core::Mesh*                                   mesh,
                                                      Core::Geometry*                               g,
                                                      const std::vector<hair_binding::HairBinder*>& binders) {
    for (auto* b : binders)
        if (b && b->hair_mesh() == mesh && !b->current_vertices().empty() &&
            b->current_vertices().size() == g->get_properties().vertexData.size())
            return &b->current_vertices();
    const auto& props = g->get_properties();
    if (!props.deformedVertexData.empty() && props.deformedVertexData.size() == props.vertexData.size())
        return &props.deformedVertexData;
    return &props.vertexData;
}

} // namespace

Ray screen_ray(Core::Camera* camera, Vec2 mousePx, Vec2 viewportPx) {
    // The engine projection is Vulkan-style (Y flipped, depth 0..1), so the
    // NDC of a pixel is simply y-down and near = 0, far = 1.
    const float x   = 2.0f * mousePx.x / viewportPx.x - 1.0f;
    const float y   = 2.0f * mousePx.y / viewportPx.y - 1.0f;
    const Mat4  inv = glm::inverse(camera->get_projection() * camera->get_view());
    Vec4        n   = inv * Vec4(x, y, 0.0f, 1.0f);
    Vec4        f   = inv * Vec4(x, y, 1.0f, 1.0f);
    n /= n.w;
    f /= f.w;
    Ray r;
    r.origin = Vec3(n);
    r.dir    = glm::normalize(Vec3(f) - Vec3(n));
    return r;
}

Hit pick(Core::Scene*                                  scene,
         Core::Camera*                                 camera,
         Vec2                                          mousePx,
         Vec2                                          viewportPx,
         const std::vector<hair_binding::HairBinder*>& binders,
         float                                         strandTolerancePx) {
    Hit best;
    best.distance = kInf;
    if (!scene || !camera || viewportPx.x <= 0.0f || viewportPx.y <= 0.0f)
        return best;

    const Ray ray = screen_ray(camera, mousePx, viewportPx);
    // World size of one pixel at unit distance (vertical fov).
    const float pxPerUnit = 2.0f * std::tan(glm::radians(camera->get_field_of_view()) * 0.5f) / viewportPx.y;
    const float tolPerUnit = strandTolerancePx * pxPerUnit;

    for (Core::Mesh* mesh : scene->get_meshes()) {
        if (!mesh || !effectively_active(mesh))
            continue;

        const Mat4 model    = mesh->get_model_matrix();
        const Mat4 invModel = glm::inverse(model);
        const Vec3 oLocal   = Vec3(invModel * Vec4(ray.origin, 1.0f));
        const Vec3 dLocal   = Vec3(invModel * Vec4(ray.dir, 0.0f)); // t stays in world units

        float meshBest = kInf;
        for (size_t gi = 0; gi < mesh->get_num_geometries(); ++gi) {
            Core::Geometry* g = mesh->get_geometry(gi);
            if (!g || !g->data_loaded())
                continue;
            Core::IMaterial* mat    = mesh->get_material(g->get_material_ID());
            const bool       strand = (mat && Core::IMaterial::is_strand_type(mat->get_type())) ||
                                !g->get_strand_offsets().empty();
            const auto&      props  = g->get_properties();
            const auto*      verts  = current_vertices(mesh, g, binders);
            const auto&      idx    = props.vertexIndex;
            if (verts->empty())
                continue;

            if (strand) {
                // World space, so the pixel tolerance is isotropic whatever the
                // model scale. Index pairs are the LINE_LIST segments.
                std::vector<Vec3> world(verts->size());
                for (size_t i = 0; i < verts->size(); ++i)
                    world[i] = Vec3(model * Vec4((*verts)[i].pos, 1.0f));
                const size_t segs = idx.empty() ? (world.size() > 0 ? world.size() - 1 : 0) : idx.size() / 2;
                for (size_t s = 0; s < segs; ++s) {
                    const uint32_t ia = idx.empty() ? (uint32_t)s : idx[2 * s];
                    const uint32_t ib = idx.empty() ? (uint32_t)s + 1 : idx[2 * s + 1];
                    if (ia >= world.size() || ib >= world.size())
                        continue;
                    meshBest = std::min(meshBest, ray_segment(ray.origin, ray.dir, world[ia], world[ib], tolPerUnit));
                }
            } else {
                // Rest-pose bounds are only trustworthy for undeformed geometry.
                if (verts == &props.vertexData && !ray_hits_aabb(oLocal, dLocal, props.minCoords, props.maxCoords, kInf))
                    continue;
                const size_t tris = idx.empty() ? verts->size() / 3 : idx.size() / 3;
                for (size_t t = 0; t < tris; ++t) {
                    const uint32_t i0 = idx.empty() ? (uint32_t)(3 * t) : idx[3 * t];
                    const uint32_t i1 = idx.empty() ? (uint32_t)(3 * t + 1) : idx[3 * t + 1];
                    const uint32_t i2 = idx.empty() ? (uint32_t)(3 * t + 2) : idx[3 * t + 2];
                    if (i0 >= verts->size() || i1 >= verts->size() || i2 >= verts->size())
                        continue;
                    meshBest = std::min(
                        meshBest, ray_triangle(oLocal, dLocal, (*verts)[i0].pos, (*verts)[i1].pos, (*verts)[i2].pos));
                }
            }
        }

        if (meshBest < best.distance) {
            best.distance = meshBest;
            best.mesh     = mesh;
            // A light's marker mesh selects the light.
            Core::Object3D* parent = mesh->get_parent();
            best.object = (parent && parent->get_type() == ObjectType::LIGHT) ? parent : mesh;
        }
    }

    if (!best.object)
        best.distance = 0.0f;
    return best;
}

} // namespace picking
