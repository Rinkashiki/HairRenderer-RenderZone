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


static void add_neighbor(std::vector<std::vector<uint32_t>>& neighbors, uint32_t a, uint32_t b) {
    auto& list = neighbors[a];

    if (std::find(list.begin(), list.end(), b) == list.end())
        list.push_back(b);
}

std::vector<DirectionalTension> Geometry::compute_mesh_tension(const std::vector<Graphics::Vertex>& deformed) {
 
    const size_t numVerts = m_properties.vertexData.size();
    std::vector<DirectionalTension> tension(numVerts, {Vec2(1.0f, 0.0f), 0.0f});

    // Build adjacency list
    std::vector<std::vector<uint32_t>> neighbours(numVerts);

    for (size_t i = 0; i < m_properties.vertexIndex.size(); i += 3) {
        uint32_t a = m_properties.vertexIndex[i + 0];
        uint32_t b = m_properties.vertexIndex[i + 1];
        uint32_t c = m_properties.vertexIndex[i + 2];

        add_neighbor(neighbours, a, b);
        add_neighbor(neighbours, a, c);

        add_neighbor(neighbours, b, a);
        add_neighbor(neighbours, b, c);

        add_neighbor(neighbours, c, a);
        add_neighbor(neighbours, c, b);
    }
    
    // Compute directional tension for each vertex
    for (size_t v = 0; v < numVerts; ++v) {
        if (neighbours[v].empty())
            continue;
        
        const Graphics::Vertex& vert = m_properties.vertexData[v];

        // --- Tension Strength ---
        float ratioSum = 0.0f;  
        for ( uint32_t n : neighbours[v]) {
            float originalLength = glm::length(vert.pos - m_properties.vertexData[n].pos);
            float deformedLength = glm::length(deformed[v].pos - deformed[n].pos);

            if (originalLength > 1e-6f) {
                ratioSum += deformedLength / originalLength;
            }
        }

        float averageRatio = ratioSum / neighbours[v].size();
        float rawTension = 1.0f - averageRatio; 
        
        tension[v].tension = m_tensionStrength * rawTension + m_tensionBias;

        // --- Tension Direction ( Right Cauchy-Green deformation tensor) ---

        if (neighbours[v].size() < 2)
            continue;

        Vec3 bitangent = glm::cross(vert.normal, vert.tangent);

        Mat2 A(0.0f); 
        Mat2 B(0.0f); 

        for (uint32_t n : neighbours[v])
        {
            Vec3 edge0_3d = m_properties.vertexData[n].pos - vert.pos;
            Vec3 edge1_3d = deformed[n].pos - deformed[v].pos;

            // Project 3D edges onto the local tangent/bitangent plane (3D -> 2D).
            Vec2 e0(glm::dot(edge0_3d, vert.tangent), glm::dot(edge0_3d, bitangent));
            Vec2 e1(glm::dot(edge1_3d, vert.tangent), glm::dot(edge1_3d, bitangent));

            A += glm::outerProduct(e1, e0);
            B += glm::outerProduct(e0, e0);
        }

        if (std::abs(glm::determinant(B)) < 1e-9f)
           continue; 

        Mat2 F = A * glm::inverse(B);   // Deformation gradient
        Mat2 C = glm::transpose(F) * F; // Right Cauchy-Green deformation tensor ( cancel out rotation)

        float Cxx = C[0][0];
        float Cyy = C[1][1];
        float Cxy = C[0][1];
          
        // Compute the principal angle of the strain tensor (direction of maximum stretch)
        float theta = 0.5f * std::atan2(2.0f * Cxy, Cxx - Cyy);
        Vec2 dir(std::cos(theta), std::sin(theta)); 

        tension[v].direction = dir;
        

    }
    return tension;
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

    // --- Mesh tension from morph deformations ---
    std::vector<DirectionalTension> tension = compute_mesh_tension(deformed);


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

    m_VAO.vbo.upload_data(deformed.data(), numVerts * sizeof(Graphics::Vertex));
}

Graphics::VertexArrays* const get_VAO(Geometry* g) {
    return &g->m_VAO;
}
Graphics::BLAS* const get_BLAS(Geometry* g) {

    return &g->m_BLAS;
}
} // namespace Core

VULKAN_ENGINE_NAMESPACE_END