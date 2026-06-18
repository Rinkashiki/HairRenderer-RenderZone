#include "application_sl.h"
#include "resource_paths.h"
#include "../scene_loader.h"

#include <engine/core/animation_json.h>
#include <engine/core/windows/windowGLFW.h>
#include <engine/engine_config.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <process.h>
#define GETPID() _getpid()
#else
#include <unistd.h>
#define GETPID() getpid()
#endif

void SLApplication::run(const std::string& animPath,
                        const std::string& scenePath,
                        const std::string& outputPath,
                        const std::string& resourcesPath,
                        int                width,
                        int                height,
                        bool               keepFrames,
                        LogLevel           logLevel,
                        MSAASamples        msaa) {
    m_animationPath = animPath;
    m_scenePath     = scenePath;
    m_outputPath    = outputPath;
    m_resourcesPath = resourcesPath;
    m_width         = width;
    m_height        = height;
    m_keepFrames    = keepFrames;
    m_msaa          = msaa;

    Logger::init(logLevel, "slviewer.log");

    // Temp dir: <os_tmpdir>/slviewer_<pid>/
    m_tempDir = std::filesystem::temp_directory_path() /
                ("slviewer_" + std::to_string(GETPID()));
    std::filesystem::create_directories(m_tempDir);

    init();

    std::cout << "[1/2] Rendering " << m_totalFrames << " frames at "
              << m_width << "x" << m_height << " ("
              << m_fps << " fps)..." << std::endl;

    const auto renderStart = std::chrono::steady_clock::now();

    while (!m_window->get_window_should_close() && m_frameIndex < m_totalFrames)
    {
        m_window->poll_events();
        tick();
        ++m_frameIndex;

        // In-place progress bar — overwrites the same line with \r.
        const int    barWidth = 30;
        const float  progress = static_cast<float>(m_frameIndex) /
                                static_cast<float>(m_totalFrames);
        const int    filled   = static_cast<int>(progress * barWidth);
        const auto   now      = std::chrono::steady_clock::now();
        const double elapsed  = std::chrono::duration<double>(now - renderStart).count();
        const double curFps   = (elapsed > 0.0) ? (m_frameIndex / elapsed) : 0.0;
        const double eta      = (curFps > 0.0)
                                    ? (m_totalFrames - m_frameIndex) / curFps
                                    : 0.0;
        const int    etaM     = static_cast<int>(eta) / 60;
        const int    etaS     = static_cast<int>(eta) % 60;

        std::fprintf(stdout,
                     "\r  [%-*.*s] %d/%d (%3d%%) %.1f fps  ETA %02d:%02d ",
                     barWidth, filled, "##############################",
                     m_frameIndex, m_totalFrames,
                     static_cast<int>(progress * 100.0f),
                     curFps, etaM, etaS);
        std::fflush(stdout);
    }
    std::cout << std::endl;

    m_capture.cleanup();

    m_renderer->shutdown(m_scene);

    std::cout << "[2/2] Encoding video with ffmpeg (this may take a moment)..."
              << std::endl;

    try {
        VideoEncoder::encode(m_tempDir, m_outputPath, m_fps, get_ffmpeg_path());
        std::cout << "Done. Video saved to: " << m_outputPath << std::endl;
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
    settings.samplesMSAA     = m_msaa;
    settings.clearColor      = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    settings.enableUI        = false;
    settings.enableRaytracing = false;

    m_renderer = new Systems::ForwardRenderer(m_window, ShadowResolution::ULTRA, settings);
    m_renderer->init();

    setup();

    m_capture.init(m_renderer->get_device(), static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
    m_renderer->set_pre_submit_callback(m_capture.get_callback());
}

void SLApplication::setup() {
    const std::string scenePath = m_scenePath.empty()
        ? (m_resourcesPath + "scenes/default.json")
        : m_scenePath;

    auto result   = scene_loader::load_scene_json(
        scenePath,
        m_resourcesPath,
        VKFW::get_engine_resources_path(),
        /*animationOverride*/ m_animationPath,
        m_renderer);

    m_scene     = result.scene;
    m_camera    = result.camera;
    m_character = result.primaryAnimated;  // may be null if scene had no animated mesh

    // Place strand (.hair) meshes on the scalp. Headless export must drive the
    // surface binders just like HairViewer, otherwise the hair renders at its raw
    // unbound groom position (off-frame) and the video comes out bald.
    setup_hair_binding(result.hairBindings);

    // Derive frame budget by re-reading the animation header (fps + duration).
    // Cheap: the JSON header is a few hundred bytes regardless of track count.
    m_totalFrames = 1;
    if (!m_animationPath.empty())
    {
        try {
            std::ifstream f(m_animationPath);
            if (f.is_open()) {
                nlohmann::json j;
                f >> j;
                const float fps      = j.value("fps", 30.0f);
                const float duration = j.value("duration", 0.0f);
                m_fps         = fps > 0.0f ? fps : 30.0f;
                m_animDt      = 1.0f / m_fps;
                if (duration > 0.0f)
                    m_totalFrames = static_cast<int>(std::round(duration * m_fps));
            }
        } catch (...) { /* keep m_totalFrames = 1 */ }
    }
}

// Build a binder for one hair/head pair, auto-loading a sidecar: the explicitly
// declared path if given, otherwise <hair file>.hbnd next to the asset.
// (Mirrors HairViewer's make_binder.)
static hair_binding::HairBinder* make_binder(Mesh* hair, Mesh* head, const std::string& declaredPath) {
    auto*       binder = new hair_binding::HairBinder(hair, head);
    std::string side   = !declaredPath.empty() ? declaredPath : (hair->get_file_route() + ".hbnd");
    if (std::filesystem::exists(side))
        binder->load(side);
    return binder;
}

void SLApplication::setup_hair_binding(const std::vector<scene_loader::HairBindRequest>& requests) {
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

void SLApplication::tick() {
    for (Mesh* mesh : m_scene->get_meshes())
        mesh->advance_animation(m_animDt);

    // Reconstruct surface-bound hair from the (now-deformed) head surface.
    for (auto* binder : m_binders)
        binder->update();

    m_renderer->render(m_scene);

    m_capture.wait_and_write(static_cast<uint32_t>(m_frameIndex), m_tempDir);
}
