#ifndef __HAIR_BINDING__
#define __HAIR_BINDING__

#include <engine/core/scene/mesh.h>
#include <string>
#include <vector>

USING_VULKAN_ENGINE_NAMESPACE

// ─────────────────────────────────────────────────────────────────────────────
// Hair-to-scalp surface binding (.hair).
//
// Binds a strand hair mesh to a character head's surface so the hair sits on the
// skin (no clip / no float) and follows the head's morph + skeletal animation
// while keeping each strand's silhouette (rigid-per-strand follow).
//
// Technique — barycentric groom binding: each strand root is projected onto the
// nearest head triangle (rest pose). Per strand we store the triangle, the
// barycentric coords of the projection, and every strand vertex expressed in the
// bind-pose root frame (interpolated tangent/normal basis). Per frame the frame
// is rebuilt from the *deformed* triangle and the rigid delta is applied.
//
// All math is done in the head mesh's local space H. After binding, the hair
// mesh is reparented under the head with an identity local transform, so its
// model matrix equals the head's and the reconstructed (H-space) vertices land
// correctly in the world. Skeletal + morph motion arrives through the head's
// deformed surface vertices (computed CPU-side every frame by apply_deformation).
// ─────────────────────────────────────────────────────────────────────────────
namespace hair_binding {

class HairBinder {
  public:
    HairBinder(Core::Mesh* hairMesh, Core::Mesh* headMesh);

    // Project roots onto the head rest-pose surface using the hair mesh's current
    // transform as the gross alignment. normalOffset lifts roots along the
    // surface normal (units = head-local space; 0 = exactly on the skin). When
    // declip is set, every strand vertex is clamped to the root's scalp plane so
    // bodies that dip below the (possibly fatter) head are lifted onto it.
    // Returns false if either mesh lacks the required geometry.
    bool bind(float normalOffset = 0.0f, bool declip = true);

    // Reconstruct hair vertices for the head's current pose and upload them.
    // No-op until bound. Cheap-skips when neither head nor hair needs updating.
    void update();

    // Sidecar IO (binary). save() requires a prior bind(); load() rebuilds the
    // binding for the bound mesh pair and performs the initial reconstruction.
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    bool        is_bound() const { return m_bound; }
    Core::Mesh* hair_mesh() const { return m_hair; }
    Core::Mesh* head_mesh() const { return m_head; }
    float       normal_offset() const { return m_normalOffset; }

  private:
    // Per-strand binding to a head triangle.
    struct StrandBind {
        uint32_t tri[3];  // head vertex indices of the bound triangle
        float    bary[3]; // barycentric coords of the root projection
    };

    Core::Mesh* m_hair = nullptr;
    Core::Mesh* m_head = nullptr;

    std::vector<StrandBind>       m_strands;     // one per strand
    std::vector<Vec3>             m_localPos;    // per hair vertex, in bind root frame
    std::vector<Vec3>             m_localTangent;// per hair vertex, in bind root frame
    std::vector<Graphics::Vertex> m_workVerts;   // scratch buffer reused each frame

    float m_normalOffset      = 0.0f;
    bool  m_bound             = false;
    bool  m_reconstructedOnce = false;

    // Reparent hair under head with identity local transform (model == head).
    void anchor_to_head();
};

} // namespace hair_binding

#endif
