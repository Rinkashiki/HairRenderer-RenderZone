#include <engine/core/passes/variance_shadow_pass.h>

VULKAN_ENGINE_NAMESPACE_BEGIN
using namespace Graphics;
namespace Core {

void VarianceShadowPass::setup_attachments(std::vector<Graphics::AttachmentInfo>& attachments, std::vector<Graphics::SubPassDependency>& dependencies) {

    attachments.resize(2);

    attachments[0] = Graphics::AttachmentInfo(m_format,
                                              1,
                                              LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              IMAGE_USAGE_COLOR_ATTACHMENT | IMAGE_USAGE_SAMPLED,
                                              COLOR_ATTACHMENT,
                                              ASPECT_COLOR,
                                              TEXTURE_2D_ARRAY,
                                              FILTER_LINEAR,
                                              ADDRESS_MODE_CLAMP_TO_BORDER);

    attachments[1] = Graphics::AttachmentInfo(m_depthFormat,
                                              1,
                                              LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                              LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                              IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT,
                                              DEPTH_ATTACHMENT,
                                              ASPECT_DEPTH,
                                              TEXTURE_2D_ARRAY);

    // Depdencies
    dependencies.resize(2);

    dependencies[0] = Graphics::SubPassDependency(STAGE_COLOR_ATTACHMENT_OUTPUT, STAGE_COLOR_ATTACHMENT_OUTPUT, ACCESS_COLOR_ATTACHMENT_WRITE);
    dependencies[1] = Graphics::SubPassDependency(STAGE_EARLY_FRAGMENT_TESTS, STAGE_EARLY_FRAGMENT_TESTS, ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE);

    m_isResizeable = false;
}
void VarianceShadowPass::setup_uniforms(std::vector<Graphics::Frame>& frames) {

    m_descriptorPool = m_device->create_descriptor_pool(ENGINE_MAX_OBJECTS, ENGINE_MAX_OBJECTS, ENGINE_MAX_OBJECTS, ENGINE_MAX_OBJECTS, ENGINE_MAX_OBJECTS);
    m_descriptors.resize(frames.size());

    // GLOBAL SET
    LayoutBinding camBufferBinding(UNIFORM_DYNAMIC_BUFFER, SHADER_STAGE_VERTEX | SHADER_STAGE_GEOMETRY | SHADER_STAGE_FRAGMENT, 0);
    LayoutBinding sceneBufferBinding(UNIFORM_DYNAMIC_BUFFER, SHADER_STAGE_VERTEX | SHADER_STAGE_GEOMETRY | SHADER_STAGE_FRAGMENT, 1);
    LayoutBinding shadowBinding(UNIFORM_COMBINED_IMAGE_SAMPLER, SHADER_STAGE_FRAGMENT, 2);
    LayoutBinding envBinding(UNIFORM_COMBINED_IMAGE_SAMPLER, SHADER_STAGE_FRAGMENT, 3);
    LayoutBinding iblBinding(UNIFORM_COMBINED_IMAGE_SAMPLER, SHADER_STAGE_FRAGMENT, 4);
    m_descriptorPool.set_layout(GLOBAL_LAYOUT, {camBufferBinding, sceneBufferBinding, shadowBinding, envBinding, iblBinding});

    // PER-OBJECT SET
    LayoutBinding objectBufferBinding(UNIFORM_DYNAMIC_BUFFER, SHADER_STAGE_VERTEX | SHADER_STAGE_GEOMETRY | SHADER_STAGE_FRAGMENT, 0);
    LayoutBinding materialBufferBinding(UNIFORM_DYNAMIC_BUFFER, SHADER_STAGE_VERTEX | SHADER_STAGE_GEOMETRY | SHADER_STAGE_FRAGMENT, 1);
    m_descriptorPool.set_layout(OBJECT_LAYOUT, {objectBufferBinding, materialBufferBinding});

    for (size_t i = 0; i < frames.size(); i++)
    {
        // Global
        m_descriptorPool.allocate_descriptor_set(GLOBAL_LAYOUT, &m_descriptors[i].globalDescritor);
        m_descriptorPool.set_descriptor_write(
            &frames[i].uniformBuffers[0], sizeof(CameraUniforms), 0, &m_descriptors[i].globalDescritor, UNIFORM_DYNAMIC_BUFFER, 0);
        m_descriptorPool.set_descriptor_write(&frames[i].uniformBuffers[0],
                                              sizeof(SceneUniforms),
                                              m_device->pad_uniform_buffer_size(sizeof(CameraUniforms)),
                                              &m_descriptors[i].globalDescritor,
                                              UNIFORM_DYNAMIC_BUFFER,
                                              1);

        // Per-object
        m_descriptorPool.allocate_descriptor_set(OBJECT_LAYOUT, &m_descriptors[i].objectDescritor);
        m_descriptorPool.set_descriptor_write(
            &frames[i].uniformBuffers[1], sizeof(ObjectUniforms), 0, &m_descriptors[i].objectDescritor, UNIFORM_DYNAMIC_BUFFER, 0);
        m_descriptorPool.set_descriptor_write(&frames[i].uniformBuffers[1],
                                              sizeof(MaterialUniforms),
                                              m_device->pad_uniform_buffer_size(sizeof(ObjectUniforms)),
                                              &m_descriptors[i].objectDescritor,
                                              UNIFORM_DYNAMIC_BUFFER,
                                              1);
    }
}
void VarianceShadowPass::setup_shader_passes() {

    // DEPTH PASSES

    PipelineSettings        settings{};
    GraphicPipelineSettings gfxSettings{};
    settings.descriptorSetLayoutIDs = {{GLOBAL_LAYOUT, true}, {OBJECT_LAYOUT, true}, {OBJECT_TEXTURE_LAYOUT, false}};
    gfxSettings.attributes          = {
        {POSITION_ATTRIBUTE, true}, {NORMAL_ATTRIBUTE, false}, {UV_ATTRIBUTE, false}, {TANGENT_ATTRIBUTE, false}, {COLOR_ATTRIBUTE, false}};
    gfxSettings.dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                 VK_DYNAMIC_STATE_SCISSOR,
                                 VK_DYNAMIC_STATE_DEPTH_BIAS,
                                 VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                                 VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                 VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                 VK_DYNAMIC_STATE_CULL_MODE};
    // settings.blendAttachments       = {};

    GraphicShaderPass* depthPass =
        new GraphicShaderPass(m_device->get_handle(), m_renderpass, m_imageExtent, get_engine_resources_path() + "shaders/shadows/vsm_geom.glsl");
    depthPass->settings        = settings;
    depthPass->graphicSettings = gfxSettings;
    depthPass->build_shader_stages();
    depthPass->build(m_descriptorPool);
    m_shaderPasses[0] = depthPass;

    GraphicShaderPass* depthLinePass =
        new GraphicShaderPass(m_device->get_handle(), m_renderpass, m_imageExtent, get_engine_resources_path() + "shaders/shadows/vsm_line_geom.glsl");
    depthLinePass->settings                    = settings;
    depthLinePass->graphicSettings             = gfxSettings;
    depthLinePass->graphicSettings.topology    = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    depthLinePass->graphicSettings.poligonMode = VK_POLYGON_MODE_LINE;
    depthLinePass->build_shader_stages();
    depthLinePass->build(m_descriptorPool);
    m_shaderPasses[1] = depthLinePass;
}

void VarianceShadowPass::render(Graphics::Frame& currentFrame, Scene* const scene, uint32_t presentImageIndex) {
    PROFILING_EVENT()
    if ((Core::Light::get_non_raytraced_count() == 0 && currentFrame.index > 0) || scene->get_lights().empty())
        return;

    CommandBuffer cmd = currentFrame.commandBuffer;

    // Cross-frame serialization of the (single-buffered) shadow depth attachment.
    // The renderer runs multiple frames in flight (BufferingType::DOUBLE) but this
    // shadow target is a single shared image. Without a barrier, frame N+1 begins
    // clearing/writing the depth (and thus the moments produced from it) while
    // frame N's shadow pass is still resolving its depth -> torn depth -> corrupted
    // moments -> flickering self-shadows, visible only on ANIMATED geometry (a
    // static shadow is identical every frame, so the tearing is invisible). Make
    // this frame's depth write wait for the previous frame's depth write. Gating
    // the depth (earliest write in the pass) also gates the moment color that
    // follows it; the moment *read* in the forward pass is already covered by the
    // renderpass layout transition, so no extra color barrier is needed.
    Image& depthImg = m_framebuffers[0].attachmentImages[1];
    if (depthImg.currentLayout == LAYOUT_UNDEFINED)
        cmd.pipeline_barrier(depthImg,
                             LAYOUT_UNDEFINED,
                             LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             ACCESS_NONE,
                             ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE,
                             STAGE_TOP_OF_PIPE,
                             STAGE_EARLY_FRAGMENT_TESTS,
                             ASPECT_DEPTH);
    else
        cmd.pipeline_barrier(depthImg,
                             LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE,
                             ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE,
                             STAGE_LATE_FRAGMENT_TESTS,
                             STAGE_EARLY_FRAGMENT_TESTS,
                             ASPECT_DEPTH);

    cmd.begin_renderpass(m_renderpass, m_framebuffers[0]);
    cmd.set_viewport(m_imageExtent);

    cmd.set_depth_bias_enable(true);
    float depthBiasConstant = 0.0;
    float depthBiasSlope    = 0.0f;
    cmd.set_depth_bias(depthBiasConstant, 0.0f, depthBiasSlope);

    // draw_idx counts (mesh,geometry) pairs — see ResourceManager::update_object_data
    // for the canonical advancement rule (per-geometry on live meshes, 0 on null).
    int draw_idx = 0;
    for (Mesh* m : scene->get_meshes())
    {
        const size_t numGeoms = m ? m->get_num_geometries() : 0;
        if (m)
        {
            if (m->is_active() && m->cast_shadows() && numGeoms > 0)
            {
                for (size_t i = 0; i < numGeoms; i++)
                {
                    uint32_t objectOffset = currentFrame.uniformBuffers[1].strideSize * (draw_idx + i);

                    // Setup per object render state
                    Geometry*  g   = m->get_geometry(i);
                    IMaterial* mat = m->get_material(g->get_material_ID());

                    // Do NOT render strand hair into the shadow map. Hair rendered as
                    // lines into the VSM self-shadows the hair with its own strands
                    // when the hair samples the map back — thin dark streaks (hair-on-
                    // hair shadow-map acne, visible after the voxel self-shadow path
                    // was disabled in hair_strand_epic.glsl). The head/body still cast,
                    // so head->hair shadowing is preserved. Trade-off: hair no longer
                    // casts a shadow-map shadow onto the face/body (acceptable; the
                    // proper way to keep that is VSM receiver bias tuning — future work).
                    if (IMaterial::is_epic_hair_family(mat->get_type()) ||
                        mat->get_type() == IMaterial::Type::HAIR_STR_TYPE)
                        continue;

                    ShaderPass* shaderPass = mat->get_type() != IMaterial::Type::HAIR_STR_EPIC_TYPE ? m_shaderPasses[0] : m_shaderPasses[1];

                    cmd.set_depth_test_enable(mat->get_parameters().depthTest);
                    cmd.set_depth_write_enable(mat->get_parameters().depthWrite);
                    cmd.set_cull_mode(mat->get_parameters().faceCulling ? mat->get_parameters().culling : CullingMode::NO_CULLING);

                    cmd.bind_shaderpass(*shaderPass);
                    // GLOBAL LAYOUT BINDING
                    cmd.bind_descriptor_set(m_descriptors[currentFrame.index].globalDescritor, 0, *shaderPass, {0, 0});
                    // PER OBJECT LAYOUT BINDING
                    cmd.bind_descriptor_set(m_descriptors[currentFrame.index].objectDescritor, 1, *shaderPass, {objectOffset, objectOffset});

                    // DRAW
                    cmd.draw_geometry(*get_VAO(g));
                }
            }
        }
        draw_idx += numGeoms;
    }

    cmd.end_renderpass(m_renderpass, m_framebuffers[0]);
}

} // namespace Core

VULKAN_ENGINE_NAMESPACE_END