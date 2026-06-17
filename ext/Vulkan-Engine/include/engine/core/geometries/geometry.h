/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License

    Copyright (c) 2023 Antonio Espinosa Garcia

*/

#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <engine/common.h>
#include <engine/graphics/accel.h>
#include <engine/graphics/vao.h>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

class Geometry;

// Per-vertex skinning data (4 joints per vertex). Stored separately from the
// Vertex struct so all existing VAO/pipeline code is untouched. Uploaded as
// SSBOs when skeletal animation is implemented.
struct SkinData {
    std::vector<glm::uvec4> jointIndices;        // 4 joint indices per vertex
    std::vector<Vec4>       jointWeights;        // 4 normalized weights per vertex
    std::vector<Mat4>       inverseBindMatrices; // one per joint
    std::vector<Mat4>       bindLocalMatrices;   // bind-pose local TRS per joint (from GLB node)
    std::vector<std::string> jointNames;         // for debugging
    std::vector<int>        parentIndices;       // one per joint, -1 for roots
};

// Per-mesh morph target data. Stores only POSITION deltas (as exported from
// glTF). Normal deltas can be added if the source ever provides them.
struct MorphTargetData {
    struct Target {
        std::vector<Vec3> deltaPos;
    };
    std::vector<Target>      targets;
    std::vector<std::string> targetNames;
};

struct GeometricData {
    std::vector<uint32_t>         vertexIndex;
    std::vector<Graphics::Vertex> vertexData;
    std::vector<Graphics::Voxel>  voxelData;

    // Stats
    Vec3 maxCoords;
    Vec3 minCoords;
    Vec3 center;

    float avgFiberLength = 0.0f; // If fiber;

    bool loaded{false};

    // Optional skinning and morph-target data (present only for GLB meshes)
    std::optional<SkinData>       skinData;
    std::optional<MorphTargetData> morphTargetData;

    // CPU copy of the last deformed vertex buffer (positions/normals/tangents
    // after morph + skin), retained by apply_deformation() so surface-bound hair
    // can read the head's animated surface. Empty until deformation runs at least
    // once; consumers should fall back to vertexData (rest pose) when empty.
    std::vector<Graphics::Vertex> deformedVertexData;

    // For strand (.hair) geometry: start vertex index of each strand, with a
    // trailing sentinel equal to the total vertex count, so strand s spans
    // [strandVertexOffsets[s], strandVertexOffsets[s+1]). Empty for non-hair.
    std::vector<uint32_t> strandVertexOffsets;

    // Force a CPU-writable (CPU_TO_GPU) VBO even without skin/morph data, so the
    // buffer can be re-uploaded per frame (surface-bound hair). Disables BLAS.
    bool forceAnimatable = false;

    void compute_statistics();
};

/*
Class that defines the mesh geometry. Can be setup by filling it with a canonical vertex type array.
*/
class Geometry
{

  private:
    Graphics::VAO  m_VAO  = {};
    Graphics::BLAS m_BLAS = {};

    GeometricData m_properties = {};
    size_t        m_materialID = 0;

    friend Graphics::VertexArrays* const get_VAO(Geometry* g);
    friend Graphics::BLAS* const         get_BLAS(Geometry* g);

  public:
    Geometry() {
    }

    inline size_t get_material_ID() const {
        return m_materialID;
    }
    inline void set_material_ID(size_t id) {
        m_materialID = id;
    }

    inline bool data_loaded() const {
        return m_properties.loaded;
    }
    inline bool indexed() const {
        return !m_properties.vertexIndex.empty();
    }

    inline const GeometricData& get_properties() const {
        return m_properties;
    }
    inline void set_avg_fiber_length(float length) {
        m_properties.avgFiberLength = length;
    };

    inline void set_skin_data(SkinData sd) {
        m_properties.skinData = std::move(sd);
    }
    inline void set_morph_target_data(MorphTargetData md) {
        m_properties.morphTargetData = std::move(md);
    }
    // CPU copy of the last deformed vertex buffer (empty if never deformed).
    inline const std::vector<Graphics::Vertex>& get_deformed_vertices() const {
        return m_properties.deformedVertexData;
    }
    // Per-strand vertex ranges for .hair geometry (see GeometricData).
    inline void set_strand_offsets(std::vector<uint32_t> offsets) {
        m_properties.strandVertexOffsets = std::move(offsets);
    }
    inline const std::vector<uint32_t>& get_strand_offsets() const {
        return m_properties.strandVertexOffsets;
    }
    // Force a CPU-writable VBO (for surface-bound hair re-uploaded per frame).
    inline void set_animatable(bool op) {
        m_properties.forceAnimatable = op;
    }
    inline bool is_animatable() const {
        return m_properties.forceAnimatable || m_properties.skinData.has_value() || m_properties.morphTargetData.has_value();
    }
    // Re-upload an externally deformed vertex buffer to the GPU. VBO must be
    // animatable (see set_animatable). Used by the hair surface binder. Returns
    // false (no-op) if the VBO is not yet on the GPU or the buffer is empty.
    bool upload_vertices(const std::vector<Graphics::Vertex>& verts);

    // Recompute the cached AABB stats (min/max/center) from an external vertex
    // set. Used after surface-binding moves hair into a new space so the mesh
    // bounding volume (and frustum culling) reflects the bound positions.
    void update_bounds(const std::vector<Graphics::Vertex>& verts);
    /*
    Use Voxel Acceleration Structure
    */
    inline bool create_voxel_AS() const {
        return m_BLAS.topology == AccelGeometryType::AABBs;
    }
    inline void create_voxel_AS(bool op) {
        m_BLAS.topology = op ? AccelGeometryType::AABBs : AccelGeometryType::TRIANGLES;
    }
    /*
    Query if Acceleration Structure is dynamic. That means that AS will update the positions of the primitives.
    */
    inline bool dynamic_AS() const {
        return m_BLAS.dynamic;
    }
    /*
    Set if Acceleration Structure is dynamic. That means that AS will update the positions of the primitives.
    */
    inline void dynamic_AS(bool op) {
        m_BLAS.dynamic = op;
    }
    ~Geometry() {
    }

    void             fill(std::vector<Graphics::Vertex> vertexInfo);
    void             fill(std::vector<Graphics::Vertex> vertexInfo, std::vector<uint32_t> vertexIndex);
    void             fill(Vec3* pos, Vec3* normal, Vec2* uv, Vec3* tangent, uint32_t vertNumber);
    void             fill_voxel_array(std::vector<Graphics::Voxel> voxels);
    // Apply morph targets and/or skeletal skinning, then upload. VBO must be animatable (CPU_TO_GPU).
    void             apply_deformation(const std::vector<float>& morphWeights, const std::vector<Mat4>& jointMatrices);
    static Geometry* create_quad();
    static Geometry* create_cube();
};

Graphics::VertexArrays* const get_VAO(Geometry* g);
Graphics::BLAS* const         get_BLAS(Geometry* g);

} // namespace Core

VULKAN_ENGINE_NAMESPACE_END;

#endif // VK_GEOMETRY_H