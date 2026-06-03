/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License

    Copyright (c) 2023 Antonio Espinosa Garcia

*/
#ifndef PBR_H
#define PBR_H

#include <engine/core/materials/material.h>
#include <engine/graphics/descriptors.h>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

/// Epic's Unreal Engine 4 PBR Metallic-Roughness Workflow
class PhysicallyBasedMaterial : public IMaterial
{
  protected:
    Vec2 m_tileUV = {1.0f, 1.0f};

    Vec4  m_albedo        = {0.5, 0.5, 0.5, 1.0}; // w for opacity
    float m_albedoWeight  = 1.0f;                 // Weight between parameter and albedo texture
    float m_opacityWeight = 0.0f;

    float m_metalness       = 0.5f;
    float m_metalnessWeight = 1.0f; // Weight between parameter and metallness texture

    float m_roughness       = 0.5f;
    float m_roughnessWeight = 1.0f; // Weight between parameter and roughness texture

    float m_occlusion       = 1.0f;
    float m_occlusionWeight = 1.0f; // Weight between parameter and occlusion texture

    Vec3  m_emissionColor     = {0.0f, 0.0f, 0.0f};
    float m_emisionWeight     = 1.0f; // Weight between parameter and emi texture
    float m_emissionIntensity = 1.0f;

    //Whether to if casts SSR
    bool m_isReflective = false;

    // Query
    bool m_hasAlbedoTexture       = false;
    bool m_hasNormalTexture       = false;
    bool m_hasRoughnessTexture    = false;
    bool m_hasMetallicTexture     = false;
    bool m_hasAOTexture           = false;
    bool m_hasEmissiveTexture     = false;
    bool m_hasMaskTexture         = false;
    bool m_hasBentNormalTexture   = false;
    bool m_hasCurvatureTexture    = false;
    bool m_hasScatteringTexture   = false;
    bool m_hasClothesMaskTexture  = false;
    bool m_hasDetailNormalTexture = false;
    bool m_hasDetailCavityTexture = false;
    int  m_maskType               = -1;

    // Layer B microdetail (pore-scale realism)
    float m_detailTiling           = 8.0f;
    float m_detailNormalStrength   = 0.5f;
    float m_cavitySpecOcclusion    = 1.0f;
    float m_dualLobeMix            = 0.0f;   // 0 = single-lobe (default); >0 = blend soft lobe in
    float m_dualLobeRoughnessSoft  = 0.55f;

    enum Textures
    {
        ALBEDO         = 0,
        NORMAL         = 1,
        MASK_ROUGHNESS = 2,
        METALNESS      = 3,
        AO             = 4,
        EMISSIVE       = 5,
        BENT_NORMAL    = 6,
        CURVATURE      = 7,
        SCATTERING     = 8,
        CLOTHES_MASK   = 9,
        DETAIL_NORMAL  = 10,
        DETAIL_CAVITY  = 11,
    };

    std::unordered_map<int, ITexture*> m_textures{{ALBEDO, nullptr},
                                                  {NORMAL, nullptr},
                                                  {MASK_ROUGHNESS, nullptr},
                                                  {METALNESS, nullptr},
                                                  {AO, nullptr},
                                                  {EMISSIVE, nullptr},
                                                  {BENT_NORMAL, nullptr},
                                                  {CURVATURE, nullptr},
                                                  {SCATTERING, nullptr},
                                                  {CLOTHES_MASK, nullptr},
                                                  {DETAIL_NORMAL, nullptr},
                                                  {DETAIL_CAVITY, nullptr}};

    std::unordered_map<int, bool> m_textureBindingState;

    virtual std::unordered_map<int, bool> get_texture_binding_state() const {
        return m_textureBindingState;
    }
    virtual void set_texture_binding_state(int id, bool state) {
        m_textureBindingState[id] = state;
    }

  public:
    virtual Graphics::MaterialUniforms                get_uniforms() const;
    virtual inline std::unordered_map<int, ITexture*> get_textures() const {
        return m_textures;
    }
    PhysicallyBasedMaterial(Vec4 albedo = Vec4(1.0f, 1.0f, 0.5f, 1.0f))
        : IMaterial(PBR_TYPE)
        , m_albedo(albedo) {
    }
    PhysicallyBasedMaterial(Vec4 albedo, MaterialSettings params)
        : IMaterial(PBR_TYPE, params)
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

    // Weight between parameter and albedo texture
    virtual inline float get_albedo_weight() const {
        return m_albedoWeight;
    }
    // Weight between parameter and albedo texture
    virtual inline void set_albedo_weight(float w) {
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
    // Weight between parameter and op texture
    virtual inline float get_opacity_weight() const {
        return m_opacityWeight;
    }
    // Weight between parameter and op texture
    virtual inline void set_opacity_weight(float w) {
        m_opacityWeight = w;
        m_isDirty       = true;
    }
    inline bool reflective() const {
        return m_isReflective;
    }
    inline void reflective(bool op) {
        m_isReflective = op;
    }

    inline float get_metalness() const {
        return m_metalness;
    }
    inline void set_metalness(float m) {
        m_metalness = m;
        m_isDirty   = true;
    }

    // Weight between parameter and metallness texture
    virtual inline float get_metalness_weight() const {
        return m_metalnessWeight;
    }
    // Weight between parameter and metallness texture
    virtual inline void set_metalness_weight(float w) {
        m_metalnessWeight = w;
        m_isDirty         = true;
    }

    inline float get_roughness() const {
        return m_roughness;
    }
    inline void set_roughness(float r) {
        m_roughness = r;
        m_isDirty   = true;
    }

    // Weight between parameter and roughness texture
    virtual inline float get_roughness_weight() const {
        return m_roughnessWeight;
    }
    // Weight between parameter and roughness texture
    virtual inline void set_roughness_weight(float w) {
        m_roughnessWeight = w;
        m_isDirty         = true;
    }

    inline float get_occlusion() const {
        return m_occlusion;
    }
    inline void set_occlusion(float r) {
        m_occlusion = r;
        m_isDirty   = true;
    }

    // Weight between parameter and occlusion texture
    virtual inline float get_occlusion_weight() const {
        return m_occlusionWeight;
    }
    // Weight between parameter and occlusion texture
    virtual inline void set_occlusion_weight(float w) {
        m_occlusionWeight = w;
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

    /*
    Sets mask texture. Support for some presets of commercial game engines.
    */
    inline ITexture* get_mask_texture() {
        return m_textures[MASK_ROUGHNESS];
    }
    inline void set_mask_texture(ITexture* t, MaskType preset) {
        m_hasMaskTexture                      = t ? true : false;
        m_textureBindingState[MASK_ROUGHNESS] = false;
        m_textures[MASK_ROUGHNESS]            = t;
        m_maskType                            = (int)preset;
        m_isDirty                             = true;
    }

    inline ITexture* get_roughness_texture() {
        return m_textures[MASK_ROUGHNESS];
    }
    inline void set_roughness_texture(ITexture* t) {
        m_hasRoughnessTexture                 = t ? true : false;
        m_textureBindingState[MASK_ROUGHNESS] = false;
        m_textures[MASK_ROUGHNESS]            = t;
        m_isDirty                             = true;
    }
    inline ITexture* get_metallic_texture() {
        return m_textures[METALNESS];
    }
    inline void set_metallic_texture(ITexture* t) {
        m_hasMetallicTexture             = t ? true : false;
        m_textureBindingState[METALNESS] = false;
        m_textures[METALNESS]            = t;
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
    inline float get_emissive_weight() const {
        return m_emisionWeight;
    };
    inline void set_emissive_weight(float w) {
        m_emisionWeight = w;
    };
    inline float get_emission_intensity() const {
        return m_emissionIntensity;
    };
    inline void set_emission_intensity(float i) {
        m_emissionIntensity = i;
    };
    inline Vec3 get_emissive_color() const {
        return m_emissionColor;
    };
    inline void set_emissive_color(Vec3 w) {
        m_emissionColor = w;
    };
    inline ITexture* get_emissive_texture() {
        return m_textures[EMISSIVE];
    }
    inline void set_emissive_texture(ITexture* t) {
        m_hasEmissiveTexture            = t ? true : false;
        m_textureBindingState[EMISSIVE] = false;
        m_textures[EMISSIVE]            = t;
        m_isDirty                       = true;
    }
    inline MaskType get_mask_type() const {
        return (MaskType)m_maskType;
    }

    inline ITexture* get_bent_normal_texture() {
        return m_textures[BENT_NORMAL];
    }
    inline void set_bent_normal_texture(ITexture* t) {
        m_hasBentNormalTexture           = t ? true : false;
        m_textureBindingState[BENT_NORMAL] = false;
        m_textures[BENT_NORMAL]          = t;
        m_isDirty                        = true;
    }

    inline ITexture* get_curvature_texture() {
        return m_textures[CURVATURE];
    }
    inline void set_curvature_texture(ITexture* t) {
        m_hasCurvatureTexture           = t ? true : false;
        m_textureBindingState[CURVATURE] = false;
        m_textures[CURVATURE]           = t;
        m_isDirty                       = true;
    }

    inline ITexture* get_scattering_texture() {
        return m_textures[SCATTERING];
    }
    inline void set_scattering_texture(ITexture* t) {
        m_hasScatteringTexture            = t ? true : false;
        m_textureBindingState[SCATTERING] = false;
        m_textures[SCATTERING]            = t;
        m_isDirty                         = true;
    }

    inline ITexture* get_clothes_mask_texture() {
        return m_textures[CLOTHES_MASK];
    }
    inline void set_clothes_mask_texture(ITexture* t) {
        m_hasClothesMaskTexture             = t ? true : false;
        m_textureBindingState[CLOTHES_MASK] = false;
        m_textures[CLOTHES_MASK]            = t;
        m_isDirty                           = true;
    }

    inline ITexture* get_detail_normal_texture() {
        return m_textures[DETAIL_NORMAL];
    }
    inline void set_detail_normal_texture(ITexture* t) {
        m_hasDetailNormalTexture             = t ? true : false;
        m_textureBindingState[DETAIL_NORMAL] = false;
        m_textures[DETAIL_NORMAL]            = t;
        m_isDirty                            = true;
    }

    inline float get_detail_tiling() const {
        return m_detailTiling;
    }
    inline void set_detail_tiling(float t) {
        m_detailTiling = t;
        m_isDirty      = true;
    }

    inline float get_detail_normal_strength() const {
        return m_detailNormalStrength;
    }
    inline void set_detail_normal_strength(float s) {
        m_detailNormalStrength = s;
        m_isDirty              = true;
    }

    inline ITexture* get_detail_cavity_texture() {
        return m_textures[DETAIL_CAVITY];
    }
    inline void set_detail_cavity_texture(ITexture* t) {
        m_hasDetailCavityTexture             = t ? true : false;
        m_textureBindingState[DETAIL_CAVITY] = false;
        m_textures[DETAIL_CAVITY]            = t;
        m_isDirty                            = true;
    }

    inline float get_cavity_spec_occlusion() const {
        return m_cavitySpecOcclusion;
    }
    inline void set_cavity_spec_occlusion(float w) {
        m_cavitySpecOcclusion = w;
        m_isDirty             = true;
    }

    inline float get_dual_lobe_mix() const {
        return m_dualLobeMix;
    }
    inline void set_dual_lobe_mix(float m) {
        m_dualLobeMix = m;
        m_isDirty     = true;
    }

    inline float get_dual_lobe_roughness_soft() const {
        return m_dualLobeRoughnessSoft;
    }
    inline void set_dual_lobe_roughness_soft(float r) {
        m_dualLobeRoughnessSoft = r;
        m_isDirty               = true;
    }
};
} // namespace Core
VULKAN_ENGINE_NAMESPACE_END
#endif