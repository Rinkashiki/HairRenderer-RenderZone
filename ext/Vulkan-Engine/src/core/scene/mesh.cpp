#include <engine/core/scene/mesh.h>

VULKAN_ENGINE_NAMESPACE_BEGIN
namespace Core {
int Mesh::m_instanceCount = 0;

void BoundingSphere::setup(Mesh* const mesh) {
    maxCoords = {0.0f, 0.0f, 0.0f};
    minCoords = {INFINITY, INFINITY, INFINITY};

    auto                 g     = mesh->get_geometry();
    const GeometricData& stats = g->get_properties();

    if (stats.maxCoords.x > maxCoords.x)
        maxCoords.x = stats.maxCoords.x;
    if (stats.maxCoords.y > maxCoords.y)
        maxCoords.y = stats.maxCoords.y;
    if (stats.maxCoords.z > maxCoords.z)
        maxCoords.z = stats.maxCoords.z;
    if (stats.minCoords.x < minCoords.x)
        minCoords.x = stats.minCoords.x;
    if (stats.minCoords.y < minCoords.y)
        minCoords.y = stats.minCoords.y;
    if (stats.minCoords.z < minCoords.z)
        minCoords.z = stats.minCoords.z;

    // center = (maxCoords + minCoords) * 0.5f;
    radius = math::length((maxCoords - minCoords) * 0.5f);
    // Step 1: Compute bounding box center and extent
    center      = (maxCoords + minCoords) * 0.5f;
    Vec3 extent = (maxCoords - minCoords) * 0.5f;

    // Step 2: Compute the largest extent (to make it a cube)
    float halfSize = std::max({extent.x, extent.y, extent.z});

    // Step 3: Expand min and max symmetrically around center
    minCoords = center - Vec3(halfSize);
    maxCoords = center + Vec3(halfSize);
}

bool BoundingSphere::is_on_frustrum(const Frustum& frustum) const

{
    // Extract effective world-space scale from the full model matrix instead of
    // the local TRS — otherwise a parented mesh (e.g. hair under a 10× character
    // root) gets a too-small radius and gets incorrectly culled.
    const Mat4  worldMat = obj->get_model_matrix();
    const float sx = math::length(Vec3(worldMat[0]));
    const float sy = math::length(Vec3(worldMat[1]));
    const float sz = math::length(Vec3(worldMat[2]));

    const Vec3  globalCenter{worldMat * Vec4(center, 1.f)};
    const float maxScale     = std::max({sx, sy, sz});
    const float globalRadius = radius * maxScale;

    return (frustum.leftFace.get_signed_distance(globalCenter) >= -globalRadius && frustum.rightFace.get_signed_distance(globalCenter) >= -globalRadius &&
            frustum.farFace.get_signed_distance(globalCenter) >= -globalRadius && frustum.nearFace.get_signed_distance(globalCenter) >= -globalRadius &&
            frustum.topFace.get_signed_distance(globalCenter) >= -globalRadius && frustum.bottomFace.get_signed_distance(globalCenter) >= -globalRadius);
}
void AABB::setup(Mesh* const mesh) {
}
bool AABB::is_on_frustrum(const Frustum& frustum) const {
    return false;
}

Geometry* Mesh::change_geometry(Geometry* g, size_t id) {
    if (m_geometry.size() < id + 1)
    {
        LOG_ERROR("Not enough geometry slots");
        return nullptr;
    }
    Geometry* old_g = m_geometry[id];
    m_geometry[id]  = g;
    return old_g;
}
IMaterial* Mesh::change_material(IMaterial* m, size_t id) {
    if (m_material.size() < id + 1)
    {
        LOG_ERROR("Not enough material slots");
        return nullptr;
    }

    IMaterial* old_m = m_material[id];
    m_material[id]   = m;
    return old_m;
}

void Mesh::set_animation(std::unique_ptr<Animation> anim) {
    m_animation = std::move(anim);
    m_localTime = 0.0f;
    m_pose      = {};
    m_worldJointMatrices.clear();
}

// Walk the joint hierarchy root-to-leaf to compose local TRS matrices into
// mesh-local world matrices. parentIndices is sorted parent[j] < j (glTF
// guarantee), so a single forward sweep is enough.
static void build_world_joint_matrices(const std::vector<Mat4>& localTRS,
                                       const std::vector<int>&  parentIndices,
                                       std::vector<Mat4>&       out) {
    const size_t nJoints = std::min(localTRS.size(), parentIndices.size());
    out.assign(nJoints, Mat4(1.0f));
    for (size_t j = 0; j < nJoints; ++j) {
        const int parent = parentIndices[j];
        if (parent >= 0 && parent < (int)nJoints)
            out[j] = out[parent] * localTRS[j];
        else
            out[j] = localTRS[j];
    }
}

void Mesh::advance_animation(float dtSeconds) {
    if (!m_animation || m_animPaused)
        return;
    m_localTime += dtSeconds;
    static const SkinData        emptySkin;
    static const MorphTargetData emptyMorphs;
    const SkinData*        skin   = &emptySkin;
    const MorphTargetData* morphs = &emptyMorphs;
    for (auto* g : m_geometry) {
        if (!g) continue;
        const auto& props = g->get_properties();
        if (props.skinData.has_value())        skin   = &*props.skinData;
        if (props.morphTargetData.has_value()) morphs = &*props.morphTargetData;
        break;
    }
    m_animation->sample(m_localTime, *skin, *morphs, m_pose);

    build_world_joint_matrices(m_pose.jointMatrices, skin->parentIndices, m_worldJointMatrices);

    for (auto* g : m_geometry) {
        if (!g) continue;
        const auto& props = g->get_properties();
        if (props.morphTargetData.has_value() || props.skinData.has_value())
            g->apply_deformation(m_pose.morphWeights, m_pose.jointMatrices);
    }
}

Mat4 Mesh::get_world_joint_matrix(const std::string& jointName) const {
    // Locate this mesh's skin. Without one, no joint to return.
    const SkinData* skin = nullptr;
    for (auto* g : m_geometry) {
        if (!g) continue;
        const auto& props = g->get_properties();
        if (props.skinData.has_value()) { skin = &*props.skinData; break; }
    }
    if (!skin) return Mat4(1.0f);

    // Lazy populate from the bind pose so attachments work for skinned meshes
    // that have no animation driving them (e.g. a static character scene).
    // advance_animation() overrides this whenever an animation is playing.
    if (m_worldJointMatrices.empty() && !skin->bindLocalMatrices.empty())
        build_world_joint_matrices(skin->bindLocalMatrices, skin->parentIndices, m_worldJointMatrices);

    for (size_t j = 0; j < skin->jointNames.size() && j < m_worldJointMatrices.size(); ++j)
        if (skin->jointNames[j] == jointName) return m_worldJointMatrices[j];
    return Mat4(1.0f);
}

Mesh* Mesh::clone() const {
    Mesh* mesh       = new Mesh();
    mesh->m_material = m_material;
    mesh->m_geometry = m_geometry;
    mesh->setup_volume();
    mesh->set_name(m_name + std::string(" clone"));
    mesh->set_transform(m_transform);
    m_instanceCount++;
    return mesh;
}

} // namespace Core
VULKAN_ENGINE_NAMESPACE_END