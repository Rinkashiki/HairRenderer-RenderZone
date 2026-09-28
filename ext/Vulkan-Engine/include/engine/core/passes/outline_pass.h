/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License

    Copyright (c) 2023 Antonio Espinosa Garcia

*/
#ifndef OUTLINE_PASS_H
#define OUTLINE_PASS_H
#include <engine/core/passes/pass.h>
#include <engine/core/passes/postprocess_pass.h>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Core {

/*
Selection outline, part 1: rasterizes only the selected object(s) into a
single-sample RG8 mask.
  R = covered by the selected object
  G = that (front-most) fragment is visible, i.e. not behind the scene depth
      written by the forward pass (its resolved LinearDepth attachment).
A mesh is outlined when it is selected, or when it is the dummy marker of a
selected light. Strand hair is drawn as screen-space quads a few pixels wide so
a groom masks as one shape instead of thousands of 1px lines. With nothing
selected the pass only clears, so the composite always reads a valid image.
*/
class OutlineMaskPass : public GraphicPass
{
    ColorFormatType m_depthFormat;

    struct FrameDescriptors {
        Graphics::DescriptorSet globalDescritor; // camera UBO + scene depth
        Graphics::DescriptorSet objectDescritor; // per-object UBO
    };
    std::vector<FrameDescriptors> m_descriptors;
    Graphics::Image               m_sceneDepth;

    bool m_hasSelection{false};

  public:
    OutlineMaskPass(Graphics::Device* ctx, Extent2D extent, ColorFormatType depthFormat)
        : BasePass(ctx, extent, 1, 1, false, "OUTLINE MASK")
        , m_depthFormat(depthFormat) {
    }

    // True when the last render() drew anything (read by the composite pass,
    // which renders right after it in the same frame).
    inline bool has_selection() const {
        return m_hasSelection;
    }

    void setup_attachments(std::vector<Graphics::AttachmentInfo>&    attachments,
                           std::vector<Graphics::SubPassDependency>& dependencies) override;
    void setup_uniforms(std::vector<Graphics::Frame>& frames) override;
    void setup_shader_passes() override;
    void render(Graphics::Frame& currentFrame, Scene* const scene, uint32_t presentImageIndex = 0) override;
    // images[0] = forward pass LinearDepth (resolved)
    void link_previous_images(std::vector<Graphics::Image> images) override;
};

/*
Selection outline, part 2: copies the tonemapped image and draws an
anti-aliased ring around the mask — solid where the object is visible, faint
where it is occluded. Sits between tonemapping and FXAA, so the outline colour
is exact (not tonemapped) and is still smoothed when FXAA is on. With no
selection it is a straight texelFetch copy.
*/
class OutlineCompositePass : public PostProcessPass
{
    // Must match the push-constant block in outline_composite.glsl.
    struct PushData {
        Vec4  color;       // linear RGB + opacity
        float width;       // outline half-width in pixels
        float hiddenAlpha; // opacity multiplier where the object is occluded
        int   active;      // 0 = passthrough
        int   _pad;
    };

    OutlineMaskPass* m_mask{nullptr};
    Vec4             m_color{1.0f, 0.36f, 0.03f, 1.0f}; // linear ≈ sRGB #FFA030
    float            m_width{2.0f};
    float            m_hiddenAlpha{0.3f};
    bool             m_enabled{true};

  public:
    OutlineCompositePass(Graphics::Device* ctx,
                         Extent2D          extent,
                         ColorFormatType   colorFormat,
                         Mesh*             vignette,
                         OutlineMaskPass*  mask,
                         bool              isDefault)
        : PostProcessPass(ctx, extent, colorFormat, vignette,
                          get_engine_resources_path() + "shaders/misc/outline_composite.glsl", "OUTLINE", isDefault)
        , m_mask(mask) {
    }

    inline Vec4 get_outline_color() const {
        return m_color;
    }
    inline void set_outline_color(Vec4 c) {
        m_color = c;
    }
    inline float get_outline_width() const {
        return m_width;
    }
    inline void set_outline_width(float w) {
        m_width = glm::clamp(w, 0.5f, 8.0f);
    }
    inline float get_hidden_alpha() const {
        return m_hiddenAlpha;
    }
    inline void set_hidden_alpha(float a) {
        m_hiddenAlpha = glm::clamp(a, 0.0f, 1.0f);
    }
    // Toggle the effect WITHOUT skipping the pass (it may be the swapchain pass).
    inline void set_outline_enabled(bool e) {
        m_enabled = e;
    }
    inline bool is_outline_enabled() const {
        return m_enabled;
    }

    void setup_uniforms(std::vector<Graphics::Frame>& frames) override;
    void setup_shader_passes() override;
    void render(Graphics::Frame& currentFrame, Scene* const scene, uint32_t presentImageIndex = 0) override;
    // images[0] = tonemapped color, images[1] = outline mask (ascending pass index)
    void link_previous_images(std::vector<Graphics::Image> images) override;
};

} // namespace Core

VULKAN_ENGINE_NAMESPACE_END

#endif
