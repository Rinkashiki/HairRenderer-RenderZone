/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License

    Copyright (c) 2023 Antonio Espinosa Garcia

*/
#ifndef LEAF_H
#define LEAF_H

#include <engine/core/materials/material.h>
#include <engine/graphics/descriptors.h>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

/// Leaf / thin-foliage material. Dielectric surface lit through the thin-surface
/// BSDF (leaf.glsl): Cook-Torrance reflection plus specular and diffuse
/// transmission through the blade.
class LeafMaterial : public IMaterial
{
  protected:
    Vec2 m_tileUV = {1.0f, 1.0f};

    Vec4  m_albedo        = {0.5f, 0.5f, 0.5f, 1.0f}; // w for opacity
    float m_albedoWeight  = 1.0f;                     // Weight between parameter and albedo texture
    float m_opacityWeight = 0.0f;                     // Weight between parameter and albedo texture alpha

    float m_roughness       = 0.5f;
    float m_roughnessWeight = 1.0f; // Weight between parameter and roughness texture

    float m_occlusion       = 1.0f;
    float m_occlusionWeight = 1.0f; // Weight between parameter and occlusion texture

    // Fraction of light transmitted specularly (clear refraction). Leaves
    // scatter rather than refract, so this is normally 0.
    float m_specTrans = 0.0f;
    // Diffuse transmission, [0,2]. 1 = half the diffuse light is reflected,
    // half transmitted through the blade.
    float m_diffTrans = 1.0f;
    // Through-blade absorption tint. The diffuse-transmission lobe is already
    // tinted by albedo, so white leaves it unchanged.
    Vec3 m_transmittance = {1.0f, 1.0f, 1.0f};
    // Disney diffusion scatter distance d (world units). Back-lit shadows are
    // averaged with the diffusion profile over this radius. 0 = off (plain PCF).
    float m_scatterDistance = 0.0f;

    bool m_hasAlbedoTexture    = false;
    bool m_hasNormalTexture    = false;
    bool m_hasRoughnessTexture = false;
    bool m_hasAOTexture        = false;

    // Channel to sample for maps that may come from a packed atlas (ORM).
    // 0=R 1=G 2=B 3=A. Default R keeps single-channel/grayscale maps working.
    int m_roughnessChannel = 0;
    int m_occlusionChannel = 0;

    enum Textures
    {
        ALBEDO    = 0,
        NORMAL    = 1,
        ROUGHNESS = 2,
        AO        = 3,
    };

    std::unordered_map<int, ITexture*> m_textures{{ALBEDO, nullptr}, {NORMAL, nullptr}, {ROUGHNESS, nullptr}, {AO, nullptr}};
    std::unordered_map<int, bool>      m_textureBindingState;

    virtual Graphics::MaterialUniforms                get_uniforms() const;
    virtual inline std::unordered_map<int, ITexture*> get_textures() const {
        return m_textures;
    }
    virtual std::unordered_map<int, bool> get_texture_binding_state() const {
        return m_textureBindingState;
    }
    virtual void set_texture_binding_state(int id, bool state) {
        m_textureBindingState[id] = state;
    }

  public:
    LeafMaterial(Vec4 albedo = Vec4(0.5f, 0.5f, 0.5f, 1.0f))
        : IMaterial(LEAF_TYPE)
        , m_albedo(albedo) {
    }

    inline Vec2 get_tile() const {
        return m_tileUV;
    }
    inline void set_tile(Vec2 tile) {
        m_tileUV  = tile;
        m_isDirty = true;
    }

    inline Vec3 get_albedo() const {
        return Vec3(m_albedo);
    }
    inline void set_albedo(Vec3 c) {
        m_albedo  = Vec4(c, m_albedo.w);
        m_isDirty = true;
    }
    inline float get_albedo_weight() const {
        return m_albedoWeight;
    }
    inline void set_albedo_weight(float w) {
        m_albedoWeight = w;
        m_isDirty      = true;
    }

    inline float get_opacity() const {
        return m_albedo.a;
    }
    inline void set_opacity(float op) {
        m_albedo.a = op;
        m_isDirty  = true;
    }
    inline float get_opacity_weight() const {
        return m_opacityWeight;
    }
    inline void set_opacity_weight(float w) {
        m_opacityWeight = w;
        m_isDirty       = true;
    }

    inline float get_roughness() const {
        return m_roughness;
    }
    inline void set_roughness(float r) {
        m_roughness = r;
        m_isDirty   = true;
    }
    inline float get_roughness_weight() const {
        return m_roughnessWeight;
    }
    inline void set_roughness_weight(float w) {
        m_roughnessWeight = w;
        m_isDirty         = true;
    }

    inline float get_occlusion() const {
        return m_occlusion;
    }
    inline void set_occlusion(float o) {
        m_occlusion = o;
        m_isDirty   = true;
    }
    inline float get_occlusion_weight() const {
        return m_occlusionWeight;
    }
    inline void set_occlusion_weight(float w) {
        m_occlusionWeight = w;
        m_isDirty         = true;
    }

    inline float get_spec_trans() const {
        return m_specTrans;
    }
    inline void set_spec_trans(float t) {
        m_specTrans = t;
        m_isDirty   = true;
    }

    inline float get_diff_trans() const {
        return m_diffTrans;
    }
    inline void set_diff_trans(float t) {
        m_diffTrans = t;
        m_isDirty   = true;
    }

    inline Vec3 get_transmittance() const {
        return m_transmittance;
    }
    inline void set_transmittance(Vec3 t) {
        m_transmittance = t;
        m_isDirty       = true;
    }

    inline float get_scatter_distance() const {
        return m_scatterDistance;
    }
    inline void set_scatter_distance(float d) {
        m_scatterDistance = d;
        m_isDirty         = true;
    }

    inline ITexture* get_albedo_texture() {
        return m_textures[ALBEDO];
    }
    inline void set_albedo_texture(ITexture* t) {
        m_hasAlbedoTexture            = t ? true : false;
        m_textureBindingState[ALBEDO] = false;
        m_textures[ALBEDO]            = t;
        m_isDirty                     = true;
    }

    inline ITexture* get_normal_texture() {
        return m_textures[NORMAL];
    }
    inline void set_normal_texture(ITexture* t) {
        m_hasNormalTexture            = t ? true : false;
        m_textureBindingState[NORMAL] = false;
        m_textures[NORMAL]            = t;
        m_isDirty                     = true;
    }

    inline ITexture* get_roughness_texture() {
        return m_textures[ROUGHNESS];
    }
    inline void set_roughness_texture(ITexture* t) {
        m_hasRoughnessTexture            = t ? true : false;
        m_textureBindingState[ROUGHNESS] = false;
        m_textures[ROUGHNESS]            = t;
        m_isDirty                        = true;
    }

    inline ITexture* get_occlusion_texture() {
        return m_textures[AO];
    }
    inline void set_occlusion_texture(ITexture* t) {
        m_hasAOTexture            = t ? true : false;
        m_textureBindingState[AO] = false;
        m_textures[AO]            = t;
        m_isDirty                 = true;
    }

    // Packed-atlas channel selection (0=R 1=G 2=B 3=A). Set alongside the texture
    // when the map is one channel of a shared image (e.g. ORM: AO=R, rough=G).
    inline void set_roughness_channel(int c) {
        m_roughnessChannel = c;
        m_isDirty          = true;
    }
    inline void set_occlusion_channel(int c) {
        m_occlusionChannel = c;
        m_isDirty          = true;
    }
};

} // namespace Core
VULKAN_ENGINE_NAMESPACE_END
#endif
