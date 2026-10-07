#include "engine/core/materials/leaf.h"

VULKAN_ENGINE_NAMESPACE_BEGIN
namespace Core {

Graphics::MaterialUniforms LeafMaterial::get_uniforms() const {
    // Uniform layout (each slot is a vec4), mirrored by leaf.glsl:
    //   dataSlot1: { albedo.r, albedo.g, albedo.b, opacity }
    //   dataSlot2: { tileU, tileV, alphaTest, blending }
    //   dataSlot3: { albedoWeight, opacityWeight, roughness, roughnessWeight }
    //   dataSlot4: { occlusion, occlusionWeight, packedChannels, _ }
    //   dataSlot5: { hasAlbedoTexture, hasNormalTexture, hasRoughnessTexture, hasAOTexture }
    //   dataSlot6: { transmittance.r, transmittance.g, transmittance.b, specTrans }
    //   dataSlot7: { diffTrans, scatterDistance, _, _ }
    // packedChannels: 2-bit channel index per packed-atlas map. Bits 0-1 rough, 2-3 AO.
    int packedChannels = (m_roughnessChannel & 3) | ((m_occlusionChannel & 3) << 2);

    Graphics::MaterialUniforms uniforms;
    uniforms.dataSlot1 = m_albedo;
    uniforms.dataSlot2 = {m_tileUV.x, m_tileUV.y, m_settings.alphaTest, m_settings.blending};
    uniforms.dataSlot3 = {m_albedoWeight, m_opacityWeight, m_roughness, m_roughnessWeight};
    uniforms.dataSlot4 = {m_occlusion, m_occlusionWeight, float(packedChannels), 0.0f};
    uniforms.dataSlot5 = {m_hasAlbedoTexture, m_hasNormalTexture, m_hasRoughnessTexture, m_hasAOTexture};
    uniforms.dataSlot6 = Vec4(m_transmittance, m_specTrans);
    uniforms.dataSlot7 = {m_diffTrans, m_scatterDistance, 0.0f, 0.0f};
    return uniforms;
}

} // namespace Core
VULKAN_ENGINE_NAMESPACE_END
