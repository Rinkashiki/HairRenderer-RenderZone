#include <engine/core/passes/dof_pass.h>

VULKAN_ENGINE_NAMESPACE_BEGIN
using namespace Graphics;
namespace Core {

// ---------------------------------------------------------------------------
// Pass setup
// ---------------------------------------------------------------------------

void DepthOfFieldPass::setup_attachments(std::vector<Graphics::AttachmentInfo>&    attachments,
                                         std::vector<Graphics::SubPassDependency>& dependencies) {
    attachments.resize(1);

    // HDR output — matches the bloom/tonemapping chain so bright bokeh keeps its
    // energy until tonemapping clamps it.
    attachments[0] = Graphics::AttachmentInfo(SRGBA_32F,
                                              1,
                                              LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              IMAGE_USAGE_COLOR_ATTACHMENT | IMAGE_USAGE_SAMPLED,
                                              COLOR_ATTACHMENT,
                                              ASPECT_COLOR,
                                              TEXTURE_2D,
                                              FILTER_LINEAR,
                                              ADDRESS_MODE_CLAMP_TO_EDGE);
    attachments[0].isDefault = false;

    dependencies.resize(2);

    dependencies[0] = Graphics::SubPassDependency(
        STAGE_FRAGMENT_SHADER, STAGE_COLOR_ATTACHMENT_OUTPUT, ACCESS_COLOR_ATTACHMENT_WRITE);
    dependencies[0].srcAccessMask   = ACCESS_SHADER_READ;
    dependencies[0].dependencyFlags = SUBPASS_DEPENDENCY_NONE;

    dependencies[1] =
        Graphics::SubPassDependency(STAGE_COLOR_ATTACHMENT_OUTPUT, STAGE_FRAGMENT_SHADER, ACCESS_SHADER_READ);
    dependencies[1].srcAccessMask   = ACCESS_COLOR_ATTACHMENT_WRITE;
    dependencies[1].srcSubpass      = 0;
    dependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
    dependencies[1].dependencyFlags = SUBPASS_DEPENDENCY_NONE;
}

void DepthOfFieldPass::setup_uniforms(std::vector<Graphics::Frame>& frames) {
    // Pool: 1 set, 1 regular UBO, 0 dynamic, 0 storage, 2 combined image samplers
    m_descriptorPool = m_device->create_descriptor_pool(1, 1, 0, 0, 2);

    // Bindings — image order in link_previous_images is sorted by ascending source
    // pass index, so FORWARD (depth) precedes BLOOM (color).
    LayoutBinding depthBinding(UNIFORM_COMBINED_IMAGE_SAMPLER, SHADER_STAGE_FRAGMENT, 0);
    LayoutBinding colorBinding(UNIFORM_COMBINED_IMAGE_SAMPLER, SHADER_STAGE_FRAGMENT, 1);
    LayoutBinding uboBinding(UNIFORM_BUFFER, SHADER_STAGE_FRAGMENT, 2);

    m_descriptorPool.set_layout(GLOBAL_LAYOUT, {depthBinding, colorBinding, uboBinding});
    m_descriptorPool.allocate_descriptor_set(GLOBAL_LAYOUT, &m_descriptorSet);

    m_ubo = m_device->create_buffer(sizeof(DoFUniforms),
                                    BUFFER_USAGE_UNIFORM_BUFFER,
                                    MEMORY_PROPERTY_HOST_VISIBLE | MEMORY_PROPERTY_HOST_COHERENT);

    DoFUniforms data{};
    data.invProjection = Mat4(1.0f);
    data.screenSize    = Vec2(static_cast<float>(m_imageExtent.width), static_cast<float>(m_imageExtent.height));
    data.focusDistance = m_focusDistance;
    data.focusRange    = m_focusRange;
    data.nearBlurScale = m_nearBlurScale;
    data.farBlurScale  = m_farBlurScale;
    data.maxCoC        = m_maxCoC;
    data.enabled       = m_dofEnabled;
    m_ubo.upload_data(&data, sizeof(DoFUniforms));

    m_descriptorPool.set_descriptor_write(&m_ubo, sizeof(DoFUniforms), 0, &m_descriptorSet, UNIFORM_BUFFER, 2);
}

void DepthOfFieldPass::setup_shader_passes() {
    GraphicShaderPass* dofPass = new GraphicShaderPass(
        m_device->get_handle(), m_renderpass, m_imageExtent, get_engine_resources_path() + "shaders/misc/dof.glsl");
    dofPass->settings.descriptorSetLayoutIDs = {{GLOBAL_LAYOUT, true}};
    dofPass->graphicSettings.attributes      = {{POSITION_ATTRIBUTE, true},
                                               {NORMAL_ATTRIBUTE, false},
                                               {UV_ATTRIBUTE, true},
                                               {TANGENT_ATTRIBUTE, false},
                                               {COLOR_ATTRIBUTE, false}};

    dofPass->build_shader_stages();
    dofPass->build(m_descriptorPool);

    m_shaderPasses[0] = dofPass;
}

// ---------------------------------------------------------------------------
// Per-frame render
// ---------------------------------------------------------------------------

void DepthOfFieldPass::render(Graphics::Frame& currentFrame, Scene* const scene, uint32_t presentImageIndex) {
    Camera* cam = scene->get_active_camera();

    DoFUniforms data{};
    data.invProjection = glm::inverse(cam->get_projection());
    data.screenSize    = Vec2(static_cast<float>(m_imageExtent.width), static_cast<float>(m_imageExtent.height));
    data.focusDistance = m_focusDistance;
    data.focusRange    = m_focusRange;
    data.nearBlurScale = m_nearBlurScale;
    data.farBlurScale  = m_farBlurScale;
    data.maxCoC        = m_maxCoC;
    data.enabled       = m_dofEnabled;
    m_ubo.upload_data(&data, sizeof(DoFUniforms));

    CommandBuffer cmd = currentFrame.commandBuffer;
    cmd.begin_renderpass(m_renderpass, m_framebuffers[0]);
    cmd.set_viewport(m_imageExtent);

    ShaderPass* shaderPass = m_shaderPasses[0];
    cmd.bind_shaderpass(*shaderPass);
    cmd.bind_descriptor_set(m_descriptorSet, 0, *shaderPass);

    Geometry* g = m_vignette->get_geometry();
    cmd.draw_geometry(*get_VAO(g));

    cmd.end_renderpass(m_renderpass, m_framebuffers[0]);
}

void DepthOfFieldPass::link_previous_images(std::vector<Graphics::Image> images) {
    // images[0] = depth (FORWARD_PASS), images[1] = color (BLOOM_PASS)
    m_depthImage = images[0];
    m_colorImage = images[1];

    m_descriptorPool.set_descriptor_write(&m_depthImage, LAYOUT_SHADER_READ_ONLY_OPTIMAL, &m_descriptorSet, 0);
    m_descriptorPool.set_descriptor_write(&m_colorImage, LAYOUT_SHADER_READ_ONLY_OPTIMAL, &m_descriptorSet, 1);
}

void DepthOfFieldPass::cleanup() {
    m_ubo.cleanup();
    BasePass::cleanup();
}

} // namespace Core
VULKAN_ENGINE_NAMESPACE_END
