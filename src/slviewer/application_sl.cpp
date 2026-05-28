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
                        LogLevel           logLevel) {
    m_animationPath = animPath;
    m_scenePath     = scenePath;
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
    settings.samplesMSAA = MSAASamples::x1;
    settings.clearColor  = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    settings.enableUI    = false;

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

void SLApplication::tick() {
    for (Mesh* mesh : m_scene->get_meshes())
        mesh->advance_animation(m_animDt);

    m_renderer->render(m_scene);

    m_capture.wait_and_write(static_cast<uint32_t>(m_frameIndex), m_tempDir);
}
