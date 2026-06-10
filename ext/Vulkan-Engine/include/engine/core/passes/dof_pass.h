/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License

    Copyright (c) 2023 Antonio Espinosa Garcia

*/
#ifndef DOF_PASS_H
#define DOF_PASS_H

#include <engine/core/passes/pass.h>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

/*
Depth of Field Pass (HDR, single-pass bokeh gather).

Runs on the HDR scene color (after bloom, before tonemapping) and the forward
pass' depth attachment. For every pixel it reconstructs the view-space distance
to the camera, derives a circle-of-confusion (CoC) radius from artist-friendly
focus parameters, then gathers neighbouring samples on a golden-angle disk to
produce round bokeh.

Inputs (link_previous_images order — sorted by ascending source pass index):
  images[0] = depth  (FORWARD_PASS, raw gl_FragCoord.z in R)
  images[1] = color  (BLOOM_PASS,   HDR scene color)

Focus model (artistic):
  - focusDistance : view-space distance to the focal plane (world units)
  - focusRange    : half-width of the in-focus band around focusDistance
  - nearBlurScale : pixels of blur gained per world unit in front of the band
  - farBlurScale  : pixels of blur gained per world unit behind the band
  - maxCoC        : hard cap on the blur radius, in pixels

When disabled the shader does a cheap passthrough (outputs the center color), so
the pass can stay active in the chain and downstream tonemapping always reads a
valid image. This mirrors SSSPass::set_active.
*/
class DepthOfFieldPass : public BasePass
{
    // Must match std140 layout declared in dof.glsl
    struct DoFUniforms {
        Mat4  invProjection; // reconstruct view-space depth from NDC depth
        Vec2  screenSize;    // render target size in pixels
        float focusDistance;
        float focusRange;
        float nearBlurScale;
        float farBlurScale;
        float maxCoC;
        int   enabled;       // 0 = passthrough, 1 = active
    };

    Mesh*                   m_vignette;
    Graphics::DescriptorSet m_descriptorSet;
    Graphics::Buffer        m_ubo;
    Graphics::Image         m_depthImage;
    Graphics::Image         m_colorImage;

    float m_focusDistance = 3.0f;
    float m_focusRange    = 0.5f;
    float m_nearBlurScale = 6.0f;
    float m_farBlurScale  = 6.0f;
    float m_maxCoC        = 16.0f;
    int   m_dofEnabled    = 0; // default OFF — existing scenes render unchanged

  public:
    DepthOfFieldPass(Graphics::Device* ctx, Extent2D extent, Mesh* vignette)
        : BasePass(ctx, extent, 1, 1, false, "DEPTH_OF_FIELD")
        , m_vignette(vignette) {
    }

    inline float get_focus_distance() const {
        return m_focusDistance;
    }
    inline void set_focus_distance(float d) {
        m_focusDistance = d;
    }
    inline float get_focus_range() const {
        return m_focusRange;
    }
    inline void set_focus_range(float r) {
        m_focusRange = glm::max(r, 0.0f);
    }
    inline float get_near_blur_scale() const {
        return m_nearBlurScale;
    }
    inline void set_near_blur_scale(float s) {
        m_nearBlurScale = glm::max(s, 0.0f);
    }
    inline float get_far_blur_scale() const {
        return m_farBlurScale;
    }
    inline void set_far_blur_scale(float s) {
        m_farBlurScale = glm::max(s, 0.0f);
    }
    inline float get_max_coc() const {
        return m_maxCoC;
    }
    inline void set_max_coc(float c) {
        m_maxCoC = glm::max(c, 0.0f);
    }

    // Toggle the effect WITHOUT skipping the pass: keep rendering (so tonemapping
    // always reads a valid image) and flip a UBO flag for shader passthrough.
    inline void set_active(const bool s) override {
        m_dofEnabled = s ? 1 : 0;
    }
    inline bool is_active() override {
        return true; // pass always runs; m_dofEnabled drives the effect
    }
    inline bool is_dof_enabled() const {
        return m_dofEnabled != 0;
    }

    void setup_attachments(std::vector<Graphics::AttachmentInfo>&    attachments,
                           std::vector<Graphics::SubPassDependency>& dependencies) override;

    void setup_uniforms(std::vector<Graphics::Frame>& frames) override;

    void setup_shader_passes() override;

    void render(Graphics::Frame& currentFrame, Scene* const scene, uint32_t presentImageIndex = 0) override;

    void link_previous_images(std::vector<Graphics::Image> images) override;

    void cleanup() override;
};

} // namespace Core
VULKAN_ENGINE_NAMESPACE_END

#endif
