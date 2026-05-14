#pragma once

#include <filesystem>
#include <functional>

#include <engine/graphics/device.h>
#include <engine/graphics/frame.h>

USING_VULKAN_ENGINE_NAMESPACE

class FrameCapture
{
  public:
    FrameCapture()  = default;
    ~FrameCapture() = default;

    void init(Graphics::Device* device, uint32_t width, uint32_t height);
    void cleanup();

    // Register this as the renderer's pre-submit callback to record copy commands.
    std::function<void(Graphics::Frame&, uint32_t)> get_callback();

    // Wait for GPU to finish the current frame, then map the staging buffer and write PNG.
    void wait_and_write(uint32_t frameIndex, const std::filesystem::path& outDir);

  private:
    Graphics::Device* m_device{nullptr};
    Graphics::Buffer  m_stagingBuffer{};
    uint32_t          m_width{0};
    uint32_t          m_height{0};
    bool              m_initialized{false};
};
