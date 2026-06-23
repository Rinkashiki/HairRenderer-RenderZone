#include <engine/core/geometries/geometry.h>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

void Geometry::fill(std::vector<Graphics::Vertex> vertexInfo) {
    m_properties.vertexData = vertexInfo;
    m_properties.compute_statistics();
    m_properties.loaded = true;
}
void Geometry::fill(std::vector<Graphics::Vertex> vertexInfo, std::vector<uint32_t> vertexIndex) {
    m_properties.vertexData  = vertexInfo;
    m_properties.vertexIndex = vertexIndex;
    m_properties.compute_statistics();
    m_properties.loaded = true;
}

void Geometry::fill(Vec3* pos, Vec3* normal, Vec2* uv, Vec3* tangent, uint32_t vertNumber) {
    for (size_t i = 0; i < vertNumber; i++)
    {
        m_properties.vertexData.push_back({pos[i], normal[i], tangent[i], uv[i], Vec3(1.0)});
    }
    m_properties.compute_statistics();
    m_properties.loaded = true;
}

void Geometry::fill_voxel_array(std::vector<Graphics::Voxel> voxels) {
    m_properties.voxelData = voxels;
}
void GeometricData::compute_statistics() {
    maxCoords = {0.0f, 0.0f, 0.0f};
    minCoords = {INFINITY, INFINITY, INFINITY};

   for (const Graphics::Vertex& v : vertexData)
    {
        if (v.pos.x > maxCoords.x)
            maxCoords.x = v.pos.x;
        if (v.pos.y > maxCoords.y)
            maxCoords.y = v.pos.y;
        if (v.pos.z > maxCoords.z)
            maxCoords.z = v.pos.z;
        if (v.pos.x < minCoords.x)
            minCoords.x = v.pos.x;
        if (v.pos.y < minCoords.y)
            minCoords.y = v.pos.y;
        if (v.pos.z < minCoords.z)
            minCoords.z = v.pos.z;
    }

    center = (maxCoords + minCoords) * 0.5f;
}

Geometry* Geometry::create_quad() {
    Geometry* g = new Geometry();

    g->fill({{{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
             {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
             {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},
             {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}},

            {0, 1, 2, 1, 3, 2});

    return g;
}

Geometry* Geometry::create_cube() {
    Geometry* g = new Geometry();

    g->fill(
        {{{-1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},  // 0
         {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}, // 1
         {{1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},  // 2
         {{1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},   // 3
         {{-1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},   // 4
         {{-1.0f, -1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},  // 5
         {{1.0f, -1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},   // 6
         {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}},   // 7
        {0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4, 0, 3, 7, 7, 4, 0, 1, 5, 6, 6, 2, 1, 0, 1, 5, 5, 4, 0, 2, 3, 7, 7, 6, 2});

    return g;
}
void Geometry::apply_deformation(const std::vector<float>& morphWeights, const std::vector<Mat4>& jointMatrices) {
    if (!m_VAO.loadedOnGPU) return;

    const size_t numVerts = m_properties.vertexData.size();
    std::vector<Graphics::Vertex> deformed = m_properties.vertexData;

    // --- Morph targets ---
    if (m_properties.morphTargetData.has_value()) {
        const auto& morphData   = *m_properties.morphTargetData;
        const size_t numTargets = std::min(morphWeights.size(), morphData.targets.size());
        for (size_t t = 0; t < numTargets; ++t) {
            const float w = morphWeights[t];
            if (std::abs(w) < 1e-6f) continue;
            const auto& target  = morphData.targets[t];
            const size_t dcount = std::min(numVerts, target.deltaPos.size());
            for (size_t v = 0; v < dcount; ++v)
                deformed[v].pos += target.deltaPos[v] * w;
        }
    }

    // --- Skeletal skinning ---
    if (m_properties.skinData.has_value() && !jointMatrices.empty()) {
        const SkinData& skin    = *m_properties.skinData;
        const size_t    nJoints = std::min(jointMatrices.size(), skin.inverseBindMatrices.size());

        // Accumulate local TRS matrices into world-space transforms, root-to-leaf.
        // parentIndices is sorted so that parent[j] < j (guaranteed by glTF hierarchy).
        std::vector<Mat4> worldMats(nJoints, Mat4(1.0f));
        for (size_t j = 0; j < nJoints; ++j) {
            int parent = (j < skin.parentIndices.size()) ? skin.parentIndices[j] : -1;
            if (parent >= 0 && parent < (int)nJoints)
                worldMats[j] = worldMats[parent] * jointMatrices[j];
            else
                worldMats[j] = jointMatrices[j];
        }

        // Build skinning matrices and their normal matrices (inverse-transpose).
        std::vector<Mat4> skinMats(nJoints);
        std::vector<Mat3> normalMats(nJoints);
        for (size_t j = 0; j < nJoints; ++j) {
            skinMats[j]   = worldMats[j] * skin.inverseBindMatrices[j];
            normalMats[j] = Mat3(glm::transpose(glm::inverse(skinMats[j])));
        }

        // Apply per-vertex blend.
        const size_t skinVerts = std::min(numVerts, skin.jointIndices.size());
        for (size_t v = 0; v < skinVerts; ++v) {
            const glm::uvec4& ji = skin.jointIndices[v];
            const Vec4&       jw = skin.jointWeights[v];

            Vec4 skinnedPos(0.0f);
            Vec3 skinnedNormal(0.0f);
            Vec3 skinnedTangent(0.0f);

            for (int k = 0; k < 4; ++k) {
                float    w = jw[k];
                uint32_t j = ji[k];
                if (w < 1e-6f || j >= nJoints) continue;
                skinnedPos     += w * (skinMats[j]   * Vec4(deformed[v].pos,    1.0f));
                skinnedNormal  += w * (normalMats[j] * deformed[v].normal);
                skinnedTangent += w * (normalMats[j] * deformed[v].tangent);
            }

            deformed[v].pos     = Vec3(skinnedPos);
            deformed[v].normal  = glm::normalize(skinnedNormal);
            deformed[v].tangent = glm::normalize(skinnedTangent);
        }
    }

    // Retain the deformed buffer so surface-bound hair can read the animated
    // head surface this frame (see GeometricData::deformedVertexData).
    m_properties.deformedVertexData = deformed;

    cycle_animatable_upload(deformed.data(), numVerts * sizeof(Graphics::Vertex));
}

void Geometry::cycle_animatable_upload(const void* data, size_t size) {
    if (m_VAO.vboCopies > 1)
    {
        // Advance to the next ring region and write there; the draw binds this
        // region via vboFrameOffset, so the GPU never reads a region the CPU is
        // mid-write on for another in-flight frame.
        m_VAO.vboWriteIndex   = (m_VAO.vboWriteIndex + 1) % m_VAO.vboCopies;
        const size_t offset   = static_cast<size_t>(m_VAO.vboWriteIndex) * m_VAO.vboCopyStride;
        m_VAO.vbo.upload_data(data, size, offset);
        m_VAO.vboFrameOffset  = static_cast<uint32_t>(offset);

        // Mirror the deformed positions into the matching posSSBO ring region so
        // the bindless consumers (hair voxelization / SSAO / SSR) follow the
        // animation instead of reading the frozen groom pose. posSSBO holds one
        // Vec4 per vertex and cycles on the same ring cursor as the VBO (different
        // stride). Both callers pass a contiguous Graphics::Vertex array, so the
        // positions are at a known offset within each element.
        if (m_VAO.posCopies > 1 && m_VAO.posCopyStride > 0)
        {
            const auto*  verts    = static_cast<const Graphics::Vertex*>(data);
            const size_t numVerts = size / sizeof(Graphics::Vertex);
            if (m_posUploadScratch.size() != numVerts)
                m_posUploadScratch.resize(numVerts);
            for (size_t v = 0; v < numVerts; ++v)
                m_posUploadScratch[v] = Vec4(verts[v].pos, 1.0f);

            const size_t posOffset = static_cast<size_t>(m_VAO.vboWriteIndex) * m_VAO.posCopyStride;
            m_VAO.posSSBO.upload_data(m_posUploadScratch.data(), numVerts * sizeof(Vec4), posOffset);
            m_VAO.posFrameOffset = static_cast<uint32_t>(posOffset);
        }
    }
    else
    {
        m_VAO.vbo.upload_data(data, size);
    }
}

bool Geometry::upload_vertices(const std::vector<Graphics::Vertex>& verts) {
    if (!m_VAO.loadedOnGPU || verts.empty()) return false;
    cycle_animatable_upload(verts.data(), verts.size() * sizeof(Graphics::Vertex));
    return true;
}

void Geometry::update_bounds(const std::vector<Graphics::Vertex>& verts) {
    if (verts.empty()) return;
    Vec3 mn(INFINITY, INFINITY, INFINITY);
    Vec3 mx(-INFINITY, -INFINITY, -INFINITY);
    for (const Graphics::Vertex& v : verts) {
        mn = glm::min(mn, v.pos);
        mx = glm::max(mx, v.pos);
    }
    m_properties.minCoords = mn;
    m_properties.maxCoords = mx;
    m_properties.center    = (mn + mx) * 0.5f;
}

Graphics::VertexArrays* const get_VAO(Geometry* g) {
    return &g->m_VAO;
}
Graphics::BLAS* const get_BLAS(Geometry* g) {

    return &g->m_BLAS;
}
} // namespace Core

VULKAN_ENGINE_NAMESPACE_END