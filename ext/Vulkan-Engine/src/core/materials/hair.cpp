#include "engine/core/materials/hair.h"

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

Graphics::MaterialUniforms HairMaterial::get_uniforms() const {
    // Alignment in shader
    //-----------------
    // vec3 sigma_a;
    // float thickness;

    // float beta;
    // float shift;
    // float ior;
    // float density;

    // float Rpower;
    // float TTpower;
    // float TRTpower;
    // bool scatter;

    // float azRoughness
    //  bool r;
    //  bool tt;
    //  bool trt;

    //-----------------

    auto deg2rad = [](float deg) { return deg / 180.0 * 3.14159265358979323846; };

    Graphics::MaterialUniforms uniforms;
    uniforms.dataSlot1 = Vec4(m_sigma_a, m_thickness);
    uniforms.dataSlot2 = {deg2rad(m_roughness), deg2rad(m_shift), m_ior, m_density};
    uniforms.dataSlot3 = {m_Rpower, m_TTpower, m_TRTpower, m_useScatter};
    uniforms.dataSlot4 = {m_azRoughness, m_R, m_TT, m_TRT};

    return uniforms;
}

Graphics::MaterialUniforms HairEpicMaterial::get_uniforms() const {
    // Alignment in shader
    //-----------------
    // vec3  baseColor;
    // float thickness;

    // float roughness;
    // float metallic;
    // float specular;
    // float shift;

    // float ior;
    // float Rpower;
    // float TTpower;
    // float TRTpower;

    // float opaqueVisibility;
    // bool  useLegacyAbsorption;
    // bool  useSeparableR;
    // bool  useBacklit;

    // bool clampBSDFValue;
    //  float r;
    // float tt;
    // float trt;
    // float SCATTER;

    //-----------------
    auto deg2rad = [](float deg) { return deg / 180.0 * 3.14159265358979323846; };

    Graphics::MaterialUniforms uniforms;
    uniforms.dataSlot1 = {Vec3(m_eumelanine, m_pheomelanine, m_tintColor.r), m_thickness};
    uniforms.dataSlot2 = {m_roughness, m_metallic, m_specular, deg2rad(m_shift)};
    uniforms.dataSlot3 = {m_ior, m_Rpower, m_TTpower, m_TRTpower};
    uniforms.dataSlot4 = {0.0, m_useLegacyAbsorption, m_useSeparableR, m_useBacklit};
    uniforms.dataSlot5 = {m_clampBSDFValue, m_R, m_TT, m_TRT};
    uniforms.dataSlot6 = {m_useScatter, m_scatterBoost, m_advancedShadowing, m_densityBoost};
    uniforms.dataSlot7 = {m_useGlints, m_rootDarkening, m_tipBleaching, m_tipFalloff};
    uniforms.dataSlot8 = {m_variabilty, m_tintColor.r, m_tintColor.g, m_tintColor.b};

    return uniforms;
}

Graphics::MaterialUniforms EyelashMaterial::get_uniforms() const {
    // Everything epic hair packs (slots 1-8) plus the eyelash-only block. Slots
    // 9-11 are unused by the epic hair path, so this adds no UBO layout change.
    //-----------------
    // float variant;        // EyelashMaterial::Variant — selects the lighting model
    // float sheenScale;     // env specular sheen damp (was hardcoded 0.15)
    // float tipTaper;       // root->tip thickness / transmission taper
    // float minPixelWidth;  // sub-pixel coverage floor
    //-----------------
    Graphics::MaterialUniforms uniforms = HairEpicMaterial::get_uniforms();

    uniforms.dataSlot9 = {float(m_variant), m_sheenScale, m_tipTaper, m_minPixelWidth};

    return uniforms;
}

} // namespace Core

VULKAN_ENGINE_NAMESPACE_END
