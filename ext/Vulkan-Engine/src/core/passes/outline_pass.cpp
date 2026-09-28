#include <engine/core/passes/outline_pass.h>

VULKAN_ENGINE_NAMESPACE_BEGIN
using namespace Graphics;
namespace Core {

// ---------------------------------------------------------------------------
// OutlineMaskPass
// ---------------------------------------------------------------------------

void OutlineMaskPass::setup_attachments(std::vector<Graphics::AttachmentInfo>&    attachments,
                                        std::vector<Graphics::SubPassDependency>& dependencies) {
    attachments.resize(2);

    // [0] Mask (R = covered, G = visible). Cleared to 0 every frame.
    attachments[0] = Graphics::AttachmentInfo(RG_8U,
                                              1,
                                              LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              IMAGE_USAGE_COLOR_ATTACHMENT | IMAGE_USAGE_SAMPLED,
                                              COLOR_ATTACHMENT,
                                              ASPECT_COLOR,
                                              TEXTURE_2D,
                                              FILTER_NEAREST,
                                              ADDRESS_MODE_CLAMP_TO_EDGE);
    attachments[0].isDefault = false;
    // AttachmentInfo's constructor writes clearValue.depthStencil.depth = 1 into
    // the VkClearValue *union*, i.e. colour R = 1 — which here would mean
    // "covered everywhere". The mask must clear to 0.
    attachments[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    // [1] Own depth, so the front-most fragment of the selected object decides
    // the visibility bit (not whichever one rasterized last).
    attachments[1] = Graphics::AttachmentInfo(m_depthFormat,
                                              1,
                                              LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                              LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                              IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT,
                                              DEPTH_ATTACHMENT,
                                              ASPECT_DEPTH,
                                              TEXTURE_2D);

    dependencies.resize(3);
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

    dependencies[2] = Graphics::SubPassDependency(
        STAGE_EARLY_FRAGMENT_TESTS, STAGE_EARLY_FRAGMENT_TESTS, ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE);
}

void OutlineMaskPass::setup_uniforms(std::vector<Graphics::Frame>& frames) {
    const uint32_t n = (uint32_t)frames.size();
    // sets, uniform buffers, dynamic uniform buffers, storage, image samplers
    m_descriptorPool = m_device->create_descriptor_pool(2 * n, 1, 2 * n, 1, n);
    m_descriptors.resize(n);

    const ShaderStageFlags allStages = SHADER_STAGE_VERTEX | SHADER_STAGE_GEOMETRY | SHADER_STAGE_FRAGMENT;

    // GLOBAL SET: camera + the scene depth the visibility bit is tested against.
    LayoutBinding camBufferBinding(UNIFORM_DYNAMIC_BUFFER, allStages, 0);
    LayoutBinding sceneDepthBinding(UNIFORM_COMBINED_IMAGE_SAMPLER, SHADER_STAGE_FRAGMENT, 1);
    m_descriptorPool.set_layout(GLOBAL_LAYOUT, {camBufferBinding, sceneDepthBinding});

    // PER-OBJECT SET: only the object block (model matrix).
    LayoutBinding objectBufferBinding(UNIFORM_DYNAMIC_BUFFER, allStages, 0);
    m_descriptorPool.set_layout(OBJECT_LAYOUT, {objectBufferBinding});

    for (uint32_t i = 0; i < n; i++)
    {
        m_descriptorPool.allocate_descriptor_set(GLOBAL_LAYOUT, &m_descriptors[i].globalDescritor);
        m_descriptorPool.set_descriptor_write(
            &frames[i].uniformBuffers[0], sizeof(CameraUniforms), 0, &m_descriptors[i].globalDescritor, UNIFORM_DYNAMIC_BUFFER, 0);

        m_descriptorPool.allocate_descriptor_set(OBJECT_LAYOUT, &m_descriptors[i].objectDescritor);
        m_descriptorPool.set_descriptor_write(
            &frames[i].uniformBuffers[1], sizeof(ObjectUniforms), 0, &m_descriptors[i].objectDescritor, UNIFORM_DYNAMIC_BUFFER, 0);
    }
}

void OutlineMaskPass::setup_shader_passes() {
    PipelineSettings        settings{};
    GraphicPipelineSettings gfxSettings{};
    settings.descriptorSetLayoutIDs = {{GLOBAL_LAYOUT, true}, {OBJECT_LAYOUT, true}, {OBJECT_TEXTURE_LAYOUT, false}};
    gfxSettings.attributes          = {
        {POSITION_ATTRIBUTE, true}, {NORMAL_ATTRIBUTE, false}, {UV_ATTRIBUTE, false}, {TANGENT_ATTRIBUTE, false}, {COLOR_ATTRIBUTE, false}};

    // Triangle meshes.
    GraphicShaderPass* meshPass = new GraphicShaderPass(
        m_device->get_handle(), m_renderpass, m_imageExtent, get_engine_resources_path() + "shaders/misc/outline_mask.glsl");
    meshPass->settings        = settings;
    meshPass->graphicSettings = gfxSettings;
    meshPass->build_shader_stages();
    meshPass->build(m_descriptorPool);
    m_shaderPasses[0] = meshPass;

    // Strand hair: LINE_LIST in, widened to quads by the geometry stage.
    GraphicShaderPass* strandPass = new GraphicShaderPass(
        m_device->get_handle(), m_renderpass, m_imageExtent, get_engine_resources_path() + "shaders/misc/outline_mask_strand.glsl");
    strandPass->settings                 = settings;
    strandPass->graphicSettings          = gfxSettings;
    strandPass->graphicSettings.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    strandPass->build_shader_stages();
    strandPass->build(m_descriptorPool);
    m_shaderPasses[1] = strandPass;
}

void OutlineMaskPass::render(Graphics::Frame& currentFrame, Scene* const scene, uint32_t presentImageIndex) {
    PROFILING_EVENT()

    CommandBuffer cmd = currentFrame.commandBuffer;
    cmd.begin_renderpass(m_renderpass, m_framebuffers[0]);
    cmd.set_viewport(m_imageExtent);

    m_hasSelection = false;

    // Same (mesh, geometry) -> object-UBO slot rule as every other scene pass
    // (see ResourceManager::update_object_data).
    int draw_idx = 0;
    for (Mesh* m : scene->get_meshes())
    {
        const size_t numGeoms = m ? m->get_num_geometries() : 0;
        if (m && numGeoms > 0 && m->is_active())
        {
            Object3D*  parent   = m->get_parent();
            const bool outlined = m->is_selected() ||
                                  (parent && parent->get_type() == ObjectType::LIGHT && parent->is_selected());
            if (outlined)
            {
                for (size_t i = 0; i < numGeoms; i++)
                {
                    Geometry*  g   = m->get_geometry(i);
                    IMaterial* mat = m->get_material(g->get_material_ID());
                    const bool strand = mat && IMaterial::is_strand_type(mat->get_type());

                    ShaderPass* shaderPass   = m_shaderPasses[strand ? 1 : 0];
                    uint32_t    objectOffset = currentFrame.uniformBuffers[1].strideSize * (draw_idx + i);

                    cmd.bind_shaderpass(*shaderPass);
                    cmd.bind_descriptor_set(m_descriptors[currentFrame.index].globalDescritor, 0, *shaderPass, {0});
                    cmd.bind_descriptor_set(m_descriptors[currentFrame.index].objectDescritor, 1, *shaderPass, {objectOffset});
                    cmd.draw_geometry(*get_VAO(g));
                }
                m_hasSelection = true;
            }
        }
        draw_idx += numGeoms;
    }

    cmd.end_renderpass(m_renderpass, m_framebuffers[0]);
}

void OutlineMaskPass::link_previous_images(std::vector<Graphics::Image> images) {
    m_sceneDepth = images[0];
    for (auto& d : m_descriptors)
        m_descriptorPool.set_descriptor_write(&m_sceneDepth, LAYOUT_SHADER_READ_ONLY_OPTIMAL, &d.globalDescritor, 1);
}

// ---------------------------------------------------------------------------
// OutlineCompositePass
// ---------------------------------------------------------------------------

void OutlineCompositePass::setup_uniforms(std::vector<Graphics::Frame>& frames) {
    m_descriptorPool = m_device->create_descriptor_pool(1, 1, 1, 1, 2);

    LayoutBinding colorBinding(UNIFORM_COMBINED_IMAGE_SAMPLER, SHADER_STAGE_FRAGMENT, 0);
    LayoutBinding maskBinding(UNIFORM_COMBINED_IMAGE_SAMPLER, SHADER_STAGE_FRAGMENT, 1);
    m_descriptorPool.set_layout(GLOBAL_LAYOUT, {colorBinding, maskBinding});
    m_descriptorPool.allocate_descriptor_set(GLOBAL_LAYOUT, &m_imageDescriptorSet);
}

void OutlineCompositePass::setup_shader_passes() {
    GraphicShaderPass* pass = new GraphicShaderPass(m_device->get_handle(), m_renderpass, m_imageExtent, m_shaderPath);
    pass->settings.descriptorSetLayoutIDs = {{GLOBAL_LAYOUT, true}};
    pass->settings.pushConstants          = {PushConstant(SHADER_STAGE_FRAGMENT, sizeof(PushData))};
    pass->graphicSettings.attributes      = {{POSITION_ATTRIBUTE, true},
                                             {NORMAL_ATTRIBUTE, false},
                                             {UV_ATTRIBUTE, false},
                                             {TANGENT_ATTRIBUTE, false},
                                             {COLOR_ATTRIBUTE, false}};
    pass->build_shader_stages();
    pass->build(m_descriptorPool);
    m_shaderPasses[0] = pass;
}

void OutlineCompositePass::render(Graphics::Frame& currentFrame, Scene* const scene, uint32_t presentImageIndex) {
    PROFILING_EVENT()

    PushData data{};
    data.color       = m_color;
    data.width       = m_width;
    data.hiddenAlpha = m_hiddenAlpha;
    data.active      = (m_enabled && m_mask && m_mask->has_selection()) ? 1 : 0;

    CommandBuffer cmd = currentFrame.commandBuffer;
    cmd.begin_renderpass(m_renderpass, m_framebuffers[m_isDefault ? presentImageIndex : 0]);
    cmd.set_viewport(m_imageExtent);

    ShaderPass* shaderPass = m_shaderPasses[0];
    cmd.bind_shaderpass(*shaderPass);
    cmd.bind_descriptor_set(m_imageDescriptorSet, 0, *shaderPass);
    cmd.push_constants(*shaderPass, SHADER_STAGE_FRAGMENT, &data, sizeof(PushData));

    Geometry* g = m_vignette->get_geometry();
    cmd.draw_geometry(*get_VAO(g));

    // Draw gui contents
    if (m_isDefault && Frame::guiEnabled)
        cmd.draw_gui_data();

    cmd.end_renderpass(m_renderpass, m_framebuffers[m_isDefault ? presentImageIndex : 0]);
}

void OutlineCompositePass::link_previous_images(std::vector<Graphics::Image> images) {
    m_descriptorPool.set_descriptor_write(&images[0], LAYOUT_SHADER_READ_ONLY_OPTIMAL, &m_imageDescriptorSet, 0);
    m_descriptorPool.set_descriptor_write(&images[1], LAYOUT_SHADER_READ_ONLY_OPTIMAL, &m_imageDescriptorSet, 1);
}

} // namespace Core
VULKAN_ENGINE_NAMESPACE_END
