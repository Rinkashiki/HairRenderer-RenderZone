#include "application_sl.h"

#include <engine/core/animation_json.h>
#include <engine/core/windows/windowGLFW.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <process.h>
#define GETPID() _getpid()
#else
#include <unistd.h>
#define GETPID() getpid()
#endif

void SLApplication::run(const std::string& animPath,
                        const std::string& outputPath,
                        const std::string& resourcesPath,
                        int                width,
                        int                height,
                        bool               keepFrames,
                        LogLevel           logLevel) {
    m_animationPath = animPath;
    m_outputPath    = outputPath;
    m_resourcesPath = resourcesPath;
    m_width         = width;
    m_height        = height;
    m_keepFrames    = keepFrames;

    Logger::init(logLevel, "slviewer.log");

    // Temp dir: <os_tmpdir>/slviewer_<pid>/
    m_tempDir = std::filesystem::temp_directory_path() /
                ("slviewer_" + std::to_string(GETPID()));
    std::filesystem::create_directories(m_tempDir);

    init();

    while (!m_window->get_window_should_close() && m_frameIndex < m_totalFrames)
    {
        m_window->poll_events();
        tick();
        ++m_frameIndex;
    }

    m_capture.cleanup();

    m_renderer->shutdown(m_scene);

    try {
        VideoEncoder::encode(m_tempDir, m_outputPath, m_fps);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Video encoding failed: ") + e.what());
        m_keepFrames = true;  // retain frames so the user can diagnose
    }

    if (!m_keepFrames)
        std::filesystem::remove_all(m_tempDir);
    else
        LOG_DEBUG("Frame dump retained at: " + m_tempDir.string());

    Logger::shutdown();
}

void SLApplication::init() {
    auto* win = new Core::WindowGLFW("SLViewer", m_width, m_height, false);
    win->set_visible_hint(false);
    m_window = win;
    m_window->init();

    Systems::RendererSettings settings{};
    settings.samplesMSAA = MSAASamples::x1;
    settings.clearColor  = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    settings.enableUI    = false;

    m_renderer = new Systems::ForwardRenderer(m_window, ShadowResolution::HIGH, settings);
    m_renderer->init();

    setup();

    m_capture.init(m_renderer->get_device(), static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
    m_renderer->set_pre_submit_callback(m_capture.get_callback());
}

void SLApplication::setup() {
    const std::string MESH_PATH    = m_resourcesPath + "models/";
    const std::string TEXTURE_PATH = m_resourcesPath + "textures/";
    const std::string ENGINE_MESH_PATH = get_engine_resources_path() + "meshes/";

    m_camera = new Camera();
    m_camera->set_position(Vec3(0.0f, 0.0f, -16.0f));
    m_camera->set_far(100.0f);
    m_camera->set_near(0.1f);
    m_camera->set_field_of_view(40.0f);

    m_scene = new Scene(m_camera);

    PointLight* light = new PointLight();
    light->set_position({-5.0f, 1.0f, -5.0f});
    light->set_shadow_fov(120.0f);
    light->set_intensity(1.0f);
    light->set_shadow_bias(0.0002f);
    light->set_shadow_near(0.1f);
    light->set_area_of_effect(30.0f);
    light->set_name("PointLight");

    Mesh* lightDummy = new Mesh();
    Tools::Loaders::load_3D_file(lightDummy, ENGINE_MESH_PATH + "sphere.obj", false);
    lightDummy->push_material(new UnlitMaterial());
    lightDummy->cast_shadows(false);
    lightDummy->set_name("LightDummy");
    light->add_child(lightDummy);
    m_scene->add(light);

    // Alex character
    m_character = new Mesh();
    std::vector<Texture*> glbTextures;
    Tools::Loaders::load_GLB(m_character, MESH_PATH + "alex/alex.glb", 0, &glbTextures);
    m_character->set_position({0.0f, -12.6f, 0.2f});
    m_character->set_scale(10.0f);
    m_character->set_rotation({0.0f, 180.0f, 0.0f});
    auto charMat = new PhysicallyBasedMaterial();
    if (!glbTextures.empty())
        charMat->set_albedo_texture(glbTextures[0]);
    charMat->set_albedo(Vec3(204.0f, 123.0f, 85.0f) / 255.0f);
    charMat->set_metalness(0.0f);
    charMat->set_roughness(0.5f);
    m_character->push_material(charMat);
    m_character->set_name("Alex");
    m_scene->add(m_character);

    // Load animation and derive frame budget
    if (Geometry* g = m_character->get_geometry(0))
    {
        const auto& props = g->get_properties();

        static const SkinData        emptySkin;
        static const MorphTargetData emptyMorphs;
        const SkinData*        skin   = props.skinData.has_value()       ? &*props.skinData       : &emptySkin;
        const MorphTargetData* morphs = props.morphTargetData.has_value() ? &*props.morphTargetData : &emptyMorphs;

        try {
            Animation anim = load_animation_json(m_animationPath, *skin, *morphs);

            m_fps         = (anim.fps > 0.0f) ? anim.fps : 30.0f;
            m_animDt      = 1.0f / m_fps;
            m_totalFrames = (anim.duration > 0.0f && anim.fps > 0.0f)
                                ? static_cast<int>(std::round(anim.duration * anim.fps))
                                : 1;

            m_character->set_animation(std::make_unique<Animation>(std::move(anim)));
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("Animation load failed: ") + e.what());
            m_totalFrames = 1;
        }
    }

    // Hair cards
    Vec3  hairOffset = Vec3{0.0f, 0.8f, 0.2f};
    Mesh* hairCards  = new Mesh();
    Tools::Loaders::load_3D_file(hairCards, MESH_PATH + "alex/hair_fauxmohawk.obj", false);
    hairCards->set_position(m_character->get_position() + hairOffset);
    hairCards->set_scale(0.1f);
    hairCards->set_rotation({0.0f, 180.0f, 0.0f});
    auto* hcMat = new HairCardMaterial();
    hcMat->set_hair_color(Vec3(0.05f, 0.02f, 0.01f));
    Texture* hairDataTex = new Texture();
    Tools::Loaders::load_texture(hairDataTex, TEXTURE_PATH + "alex/hair_fauxmohawk_attribute.png", TEXTURE_FORMAT_TYPE_NORMAL);
    hcMat->set_hair_data_texture(hairDataTex);
    Texture* hairTangentTex = new Texture();
    Tools::Loaders::load_texture(hairTangentTex, TEXTURE_PATH + "alex/hair_fauxmohawk_tangent.png", TEXTURE_FORMAT_TYPE_NORMAL);
    hcMat->set_tangent_texture(hairTangentTex);
    hairCards->push_material(hcMat);
    hairCards->set_name("HairCards");
    m_scene->add(hairCards);

    m_scene->set_ambient_color({0.05f, 0.05f, 0.05f});
    m_scene->set_ambient_intensity(0.1f);

    TextureHDR* envMap = new TextureHDR();
    Tools::Loaders::load_HDRi(envMap, TEXTURE_PATH + "studio_demo.hdr");
    Skybox* sky = new Skybox(envMap);
    sky->set_color_intensity(1.0f);
    m_scene->set_skybox(sky);
    m_scene->set_use_IBL(true);
    m_scene->enable_fog(false);

    static_cast<Systems::ForwardRenderer*>(m_renderer)->load_sss_scatter_lut(TEXTURE_PATH + "scatterDistance.png");
}

void SLApplication::tick() {
    for (Mesh* mesh : m_scene->get_meshes())
        mesh->advance_animation(m_animDt);

    m_renderer->render(m_scene);

    m_capture.wait_and_write(static_cast<uint32_t>(m_frameIndex), m_tempDir);
}
