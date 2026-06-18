#include "application.h"
#include "scene_loader.h"
#include <engine/engine_config.h>
#include <filesystem>

void HairViewer::init(Systems::RendererSettings settings) {
    m_window = new WindowGLFW("Hair Viewer", 1024, 1024);

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
    // JSON-driven scene path (default). See SCENE.md.
    auto result   = scene_loader::load_scene_json(
        SCENE_PATH,
        RESOURCES_PATH,
        VKFW::get_engine_resources_path(),
        /*animationOverride*/ "",
        m_renderer);
    m_scene  = result.scene;
    camera   = result.camera;

    m_controller = new Tools::Controller(camera, m_window, ControllerMovementType::ORBITAL);

    setup_hair_binding(result.hairBindings);
}

// Build a binder for one hair/head pair, auto-loading a sidecar: the explicitly
// declared path if given, otherwise <hair file>.hbnd next to the asset.
static hair_binding::HairBinder* make_binder(Mesh* hair, Mesh* head, const std::string& declaredPath) {
    auto* binder = new hair_binding::HairBinder(hair, head);
    std::string side = !declaredPath.empty() ? declaredPath : (hair->get_file_route() + ".hbnd");
    if (std::filesystem::exists(side))
        binder->load(side);
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
