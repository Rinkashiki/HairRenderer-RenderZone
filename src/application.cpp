#include "application.h"
#include "app_info.h"
#include "scene_loader.h"
#include <engine/engine_config.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <thread>

void HairViewer::init(Systems::RendererSettings settings) {
    m_window = new WindowGLFW(app_info::NAME, 1024, 1024);

    m_window->init();
    m_window->set_window_icon(RESOURCES_PATH "textures/icon.png");

    m_window->set_window_size_callback(std::bind(&HairViewer::window_resize_callback, this, std::placeholders::_1, std::placeholders::_2));
    m_window->set_mouse_callback(std::bind(&HairViewer::mouse_callback, this, std::placeholders::_1, std::placeholders::_2));
    m_window->set_key_callback(
        std::bind(&HairViewer::keyboard_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));

    settings.clearColor = Vec4(0.0, 0.0, 0.0, 1.0);

    // MSAA has to be known before create_passes(); peek the scene JSON for an
    // explicit renderer.msaa override. Falls back to whatever main.cpp set.
    if (auto sceneMSAA = scene_loader::peek_msaa(SCENE_PATH))
        settings.samplesMSAA = *sceneMSAA;

    m_renderer          = new Systems::ForwardRenderer(m_window, ShadowResolution::HIGH, settings); // TODO: Optimize properly to be able to use Ultra

    setup();

    m_interface.init(m_window, m_scene, m_renderer, &animateLight);
    // m_renderer->set_gui_overlay(m_interface.overlay);

    // Bind-mode panel (below the explorer). Drives the hair surface binders.
    auto* bindPanel = new Tools::Panel("HAIR BINDING", 0.0f, 0.7f, 0.2f, 0.3f, PanelWidgetFlags::NoMove, true);
    bindPanel->add_child(new HairBindWidget(&m_binders));
    m_interface.overlay->add_panel(bindPanel);

    // Everything the input callbacks touch (controller + GUI overlay) now
    // exists — safe to let window/mouse/key events through.
    m_ready = true;
}

void HairViewer::run(Systems::RendererSettings settings) {

    init(settings);
    while (!m_window->get_window_should_close())
    {
        // I-O
        m_window->poll_events();

        tick();

        if (m_maxFrames > 0 && ++m_frameCount >= m_maxFrames)
            break;
    }
    m_renderer->shutdown(m_scene);
}

void HairViewer::setup() {
    // The JSON scene load (parse + GLB geometry + decoding the 8K maps) is ~5 s
    // of pure CPU work. It touches no Vulkan/GLFW (loaders only fill CPU-side
    // caches; GPU images are created lazily at first render — the neural-hair
    // path already loads off-thread this way), so it runs on a worker. The main
    // thread meanwhile (1) brings the renderer up — device + every shader
    // compile, ~2 s that used to sit *after* the load — and then (2) draws the
    // loading screen until the worker is done. The loader is handed no renderer:
    // the scene's renderer hooks come back in LoadResult and are applied here,
    // on the main thread, so the worker never touches pass state while the
    // renderer is initialising or rendering. See SCENE.md for the schema.
    std::atomic<bool>          done{false};
    std::exception_ptr         loadError;
    scene_loader::LoadResult   result;
    LoadingProgress            progress;

    std::thread loader([&] {
        try {
            result = scene_loader::load_scene_json(
                SCENE_PATH,
                RESOURCES_PATH,
                VKFW::get_engine_resources_path(),
                /*animationOverride*/ "",
                /*renderer*/ nullptr,
                [&](float f, const std::string& stage) { progress.set(f, stage); });
        } catch (...) {
            loadError = std::current_exception();
        }
        done.store(true, std::memory_order_release);
    });

    m_renderer->init();

    // Loading screen: an empty scene (camera only, so every pass has something
    // valid to read) drawn through the full pipeline, with the splash widget on
    // a throwaway GUI overlay. The renderer's ImGui context exists from init()
    // on; the extra fonts must be registered before the first NewFrame builds
    // the atlas, and stay available to the real GUI afterwards.
    ImFont* titleFont = nullptr;
    ImFont* bodyFont  = nullptr;
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();
        titleFont = io.Fonts->AddFontFromFileTTF(RESOURCES_PATH "fonts/Roboto-Medium.ttf", 46.0f);
        bodyFont  = io.Fonts->AddFontFromFileTTF(RESOURCES_PATH "fonts/Roboto-Medium.ttf", 17.0f);
    }
    const std::string sceneName = std::filesystem::path(SCENE_PATH).stem().string();

    // Deliberately never deleted: ~Scene and ~Object3D both free the children
    // (double free), and nothing else in the app destroys a Scene either.
    Scene* splashScene = new Scene(new Camera());
    {
        Tools::GUIOverlay splash((float)m_window->get_extent().width, (float)m_window->get_extent().height);
        auto* panel = new Tools::Panel("##loading", 0.0f, 0.0f, 1.0f, 1.0f,
                                       (PanelWidgetFlags)(PanelWidgetFlags::NoDecoration | PanelWidgetFlags::NoBackground |
                                                          PanelWidgetFlags::NoInputs | PanelWidgetFlags::NoSavedSettings));
        panel->add_child(new LoadingScreenWidget(&progress, titleFont, bodyFont, "loading " + sceneName));
        splash.add_panel(panel);

        auto splashFrame = [&] {
            m_window->poll_events();
            splash.set_extent({(float)m_window->get_extent().width, (float)m_window->get_extent().height});
            splash.render();
            m_renderer->render(splashScene);
        };
        while (!done.load(std::memory_order_acquire))
            splashFrame();
        loader.join();

        if (loadError)
            std::rethrow_exception(loadError); // surface load failures as before

        // The first real frame blocks on the GPU upload (geometry + ~0.5 GB of
        // textures); say so before it does.
        progress.set(1.0f, "Uploading to GPU");
        splashFrame();
    }
    // The splash frames gave the empty scene a (blank) TLAS; drop it now or the
    // validation layer flags it as leaked at vkDestroyDevice. Nothing else in
    // it ever reached the GPU. The Scene object itself is deliberately kept.
    m_renderer->get_device()->wait();
    Core::ResourceManager::clean_scene(splashScene);

    m_scene  = result.scene;
    camera   = result.camera;

    if (auto* fwd = dynamic_cast<Systems::ForwardRenderer*>(m_renderer)) {
        if (!result.sssScatterLut.empty())
            fwd->load_sss_scatter_lut(result.sssScatterLut);
        if (result.dof.set)
            fwd->configure_dof(result.dof.enabled, result.dof.focusDistance, result.dof.focusRange,
                               result.dof.nearBlurScale, result.dof.farBlurScale, result.dof.maxCoC);
    }

    m_controller = new Tools::Controller(camera, m_window, ControllerMovementType::ORBITAL);

    setup_hair_binding(result.hairBindings);
}

// Build a binder for one hair/head pair, auto-loading a sidecar: the explicitly
// declared path if given, otherwise <hair file>.hbnd next to the asset.
static hair_binding::HairBinder* make_binder(Mesh* hair, Mesh* head, const std::string& declaredPath) {
    auto* binder = new hair_binding::HairBinder(hair, head);
    std::string side = !declaredPath.empty() ? declaredPath : (hair->get_file_route() + ".hbnd");
    binder->set_sidecar_path(side);
    // A missing sidecar leaves the binder unbound, which silently renders the
    // groom at its raw (off-frame) position — a bald character with no error.
    // Say so instead.
    if (std::filesystem::exists(side))
    {
        if (!binder->load(side))
            LOG_ERROR("hair binding: failed to load sidecar '" + side + "' for '" + hair->get_name() + "'");
    } else
        LOG_ERROR("hair binding: no sidecar at '" + side + "' — '" + hair->get_name() +
                  "' stays unbound and will render off-frame");
    return binder;
}

void HairViewer::setup_hair_binding(const std::vector<scene_loader::HairBindRequest>& requests) {
    // Scene declared explicit bindings — use them verbatim.
    if (!requests.empty()) {
        for (const auto& r : requests) {
            if (r.hair && r.head)
                m_binders.push_back(make_binder(r.hair, r.head, r.bindingPath));
        }
        return;
    }

    // Otherwise auto-discover: head = first mesh with skin/morph data.
    Mesh* head = nullptr;
    for (Mesh* m : m_scene->get_meshes()) {
        Geometry* g = m ? m->get_geometry(0) : nullptr;
        if (!g) continue;
        const auto& props = g->get_properties();
        if (props.skinData.has_value() || props.morphTargetData.has_value()) { head = m; break; }
    }
    if (!head) return; // nothing to bind to

    // Hair = every mesh whose geometry exposes per-strand ranges (.hair).
    for (Mesh* m : m_scene->get_meshes()) {
        Geometry* g = m ? m->get_geometry(0) : nullptr;
        if (!g || g->get_strand_offsets().empty() || m == head) continue;
        m_binders.push_back(make_binder(m, head, ""));
    }
}

void HairViewer::update() {
    if (!m_interface.overlay->wants_to_handle_input())
        m_controller->handle_keyboard(0, 0, m_time.delta);

    // Press P to freeze the pose: delta 0 holds the animation and the bound hair.
    const float animDelta = freezeAnimation ? 0.0f : m_time.delta;

    for (Mesh* mesh : m_scene->get_meshes())
        mesh->advance_animation(animDelta);

    // Reconstruct surface-bound hair from the (now-deformed) head surface.
    for (auto* binder : m_binders)
        binder->update();

    // Rotate the vector around the ZX plane
    auto light = m_scene->get_lights()[0];
    if (animateLight)
    {
        float rotationAngle = glm::radians(10.0f * m_time.delta);
        float _x            = light->get_position().x * cos(rotationAngle) - light->get_position().z * sin(rotationAngle);
        float _z            = light->get_position().x * sin(rotationAngle) + light->get_position().z * cos(rotationAngle);

        light->set_position({_x, light->get_position().y, _z});

        // A directional light shades purely from its direction — its position only
        // places the dummy and the shadow view. Rotating position alone would leave
        // the lighting frozen, so spin the direction by the same angle.
        if (light->get_light_type() == LightType::DIRECTIONAL)
        {
            auto* dir = static_cast<DirectionalLight*>(light);
            Vec3  d   = dir->get_direction();
            dir->set_direction({d.x * cos(rotationAngle) - d.z * sin(rotationAngle),
                                d.y,
                                d.x * sin(rotationAngle) + d.z * cos(rotationAngle)});
        }

        static_cast<UnlitMaterial*>(static_cast<Mesh*>(light->get_children().front())->get_material(0))->set_color({light->get_color() * 4.0f, 1.0f});
    }

    m_interface.objectWidget->set_object(m_interface.sceneWidget->get_selected_object());
}

void HairViewer::tick() {
    float currentTime      = (float)m_window->get_time_elapsed();
    m_time.delta           = currentTime - m_time.last;
    m_time.last            = currentTime;
    m_time.framesPerSecond = 1.0f / m_time.delta;

    update();

    m_interface.overlay->render();
    m_renderer->render(m_scene);
}
