#pragma once

#include <filesystem>
#include <string>

#include <engine/core.h>
#include <engine/systems.h>

#include "../hair_loader.h"
#include "frame_capture.h"
#include "video_encoder.h"

USING_VULKAN_ENGINE_NAMESPACE
using namespace Core;

class SLApplication
{
    Core::IWindow*         m_window{nullptr};
    Systems::BaseRenderer* m_renderer{nullptr};
    Core::Scene*           m_scene{nullptr};
    Core::Camera*          m_camera{nullptr};
    Core::Mesh*            m_character{nullptr};

    std::string m_resourcesPath;
    std::string m_animationPath;
    std::string m_outputPath{"output.mp4"};
    int         m_width{1920};
    int         m_height{1080};
    bool        m_keepFrames{false};

    int   m_totalFrames{0};
    int   m_frameIndex{0};
    float m_animDt{1.0f / 30.0f};
    float m_fps{30.0f};

    FrameCapture          m_capture;
    std::filesystem::path m_tempDir;

  public:
    void run(const std::string& animPath,
             const std::string& outputPath,
             const std::string& resourcesPath,
             int                width,
             int                height,
             bool               keepFrames,
             LogLevel           logLevel);

  private:
    void init();
    void setup();
    void tick();
};
