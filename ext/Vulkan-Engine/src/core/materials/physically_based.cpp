#include "engine/core/materials/physically_based.h"

VULKAN_ENGINE_NAMESPACE_BEGIN
namespace Core {
Graphics::MaterialUniforms PhysicallyBasedMaterial::get_uniforms() const {

    Graphics::MaterialUniforms uniforms;
    uniforms.dataSlot1 = m_albedo;
    uniforms.dataSlot2 = {m_tileUV.x, m_tileUV.y, m_settings.alphaTest, m_settings.blending};
    uniforms.dataSlot3 = {m_albedoWeight, m_metalness, m_metalnessWeight, m_roughness};
    uniforms.dataSlot4 = {m_roughnessWeight, m_occlusion, m_occlusionWeight, m_hasAlbedoTexture};
    uniforms.dataSlot5 = {m_hasNormalTexture, m_hasRoughnessTexture, m_hasMetallicTexture, m_hasAOTexture};
    uniforms.dataSlot6 = {m_hasMaskTexture, m_maskType, m_opacityWeight, m_hasEmissiveTexture};
    uniforms.dataSlot7 = {m_emissionColor, m_emisionWeight};
    // slot8.y: bit-packed material flags
    //   bit 0: isReflective       (SSR cast)
    //   bit 1: hasScatteringTexture
    //   bit 2: hasClothesMaskTexture
    int materialFlags = 0;
    if (m_isReflective)           materialFlags |= (1 << 0);
    if (m_hasScatteringTexture)   materialFlags |= (1 << 1);
    if (m_hasClothesMaskTexture)  materialFlags |= (1 << 2);

    uniforms.dataSlot8 = Vec4{m_emissionIntensity,
                              float(materialFlags),
                              m_hasCurvatureTexture ? 1.0f : 0.0f,
                              m_hasBentNormalTexture ? 1.0f : 0.0f};

    // Layer B microdetail (steps 8-11).
    //   slot9  = (detailTiling, detailNormalStrength, hasDetailNormal, hasDetailCavity)
    //   slot10 = (cavitySpecOcclusion, _, dualLobeMix, dualLobeRoughnessSoft)
    // Per-channel detail-normal blur biases (d'Eon hybrid normals) are derived
    // in-shader from the scatter-distance LUT (binding 14), not pushed via UBO.
    uniforms.dataSlot9  = Vec4{m_detailTiling,
                               m_detailNormalStrength,
                               m_hasDetailNormalTexture ? 1.0f : 0.0f,
                               m_hasDetailCavityTexture ? 1.0f : 0.0f};
    uniforms.dataSlot10 = Vec4{m_cavitySpecOcclusion,
                               0.0f, // was cavitySSSAttenuation (removed: SSS now cavity-agnostic)
                               m_dualLobeMix,
                               m_dualLobeRoughnessSoft};

    return uniforms;
}
} // namespace Core
VULKAN_ENGINE_NAMESPACE_END