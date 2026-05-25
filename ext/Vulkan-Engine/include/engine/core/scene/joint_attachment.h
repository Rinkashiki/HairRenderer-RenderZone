/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License
*/
#ifndef VK_JOINT_ATTACHMENT_H
#define VK_JOINT_ATTACHMENT_H

#include <engine/core/scene/mesh.h>
#include <engine/core/scene/object3D.h>
#include <string>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

// Empty pivot Object3D whose world matrix tracks a specific joint of a source
// Mesh. Children added via add_child() inherit the joint's world transform on
// every get_model_matrix() call, so attaching e.g. a hair mesh as a child makes
// it ride the bone for free — no per-frame app glue needed.
class JointAttachment : public Object3D {
    Mesh*       m_source = nullptr;
    std::string m_jointName;

  public:
    JointAttachment(Mesh* source, std::string jointName)
        : Object3D("JointAttachment", ObjectType::OTHER)
        , m_source(source)
        , m_jointName(std::move(jointName)) {
    }

    inline Mesh*              get_source() const { return m_source; }
    inline const std::string& get_joint_name() const { return m_jointName; }

    Mat4 get_model_matrix() override {
        if (!m_source) return Mat4(1.0f);
        return m_source->get_model_matrix() * m_source->get_world_joint_matrix(m_jointName);
    }
};

} // namespace Core

VULKAN_ENGINE_NAMESPACE_END

#endif // VK_JOINT_ATTACHMENT_H
