/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License

    Copyright (c) 2023 Antonio Espinosa Garcia

*/
#ifndef MATERIAL_H
#define MATERIAL_H

#include <engine/core/textures/texture.h>
#include <engine/graphics/shaderpass.h>
#include <engine/graphics/uniforms.h>
#include <unordered_map>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

struct MaterialSettings {
    bool        blending    = false;
    bool        faceCulling = false;
    CullingMode culling     = BACK_CULLING;
    bool        depthTest   = true;
    bool        depthWrite  = true;
    bool        alphaTest   = false;
};

class IMaterial
{
  protected:
    MaterialSettings        m_settings          = {};
    Graphics::DescriptorSet m_textureDescriptor = {};

    bool m_isDirty = true;

    friend class Renderer;

  public:
    enum Type : uint32_t
    {
        UNLIT_TYPE           = 0,
        PHONG_TYPE           = 1,
        PBR_TYPE             = 2,
        HAIR_STR_TYPE        = 3,
        HAIR_CARD_TYPE       = 4,
        HAIR_STR_DISNEY_TYPE = 5,
        HAIR_STR_EPIC_TYPE  = 6,
        // Eyelashes: same uniforms/geometry as epic strand hair, but routed to a
        // dedicated fragment shader (eyelash_strand.glsl) so they can be far less
        // reflective and more transmissive than scalp hair. Behaves like epic hair
        // in every other subsystem — use is_epic_hair_family() at those sites.
        HAIR_STR_EYELASH_TYPE = 7,
    };

    // Epic strand hair + its eyelash variant share geometry, uniforms, voxelization,
    // shadow handling and GUI — only the forward fragment shader differs. Subsystems
    // that treat "epic strand hair" specially should test the family, not the exact
    // type, or eyelashes silently drop out of voxelization / shadow / the draw path.
    static inline bool is_epic_hair_family(Type t) {
        return t == HAIR_STR_EPIC_TYPE || t == HAIR_STR_EYELASH_TYPE;
    }

    static IMaterial* DEBUG_MATERIAL;

    IMaterial(Type t)
        : m_type(t) {
    }
    IMaterial(Type t, MaterialSettings params)
        : m_type(t)
        , m_settings(params) {
    }

    ~IMaterial() {
    }

    Type get_type() const {
        return m_type;
    }

    virtual Graphics::MaterialUniforms get_uniforms() const = 0;

    virtual std::unordered_map<int, ITexture*> get_textures() const = 0;

    virtual std::unordered_map<int, bool> get_texture_binding_state() const = 0;

    virtual void set_texture_binding_state(int id, bool state) = 0;

    virtual inline MaterialSettings get_parameters() const {
        return m_settings;
    }
    virtual void set_parameters(MaterialSettings p) {
        m_settings = p;
    }

    virtual inline void set_enable_culling(bool op) {
        m_settings.faceCulling = op;
    }
    virtual inline void set_culling_type(CullingMode t) {
        m_settings.culling = t;
    }
    virtual inline void enable_depth_test(bool op) {
        m_settings.depthTest = op;
    }
    virtual inline void enable_depth_writes(bool op) {
        m_settings.depthWrite = op;
    }
    virtual inline void enable_alpha_test(bool op) {
        m_settings.alphaTest = op;
    }
    virtual inline void enable_blending(bool op) {
        m_settings.blending = op;
    }

    virtual inline Graphics::DescriptorSet& get_texture_descriptor() {
        return m_textureDescriptor;
    }
    virtual bool dirty() const {
        return m_isDirty;
    }
    virtual void dirty(bool op) {
        m_isDirty = op;
    }

  private:
    Type m_type;
};

} // namespace Core
VULKAN_ENGINE_NAMESPACE_END
#endif