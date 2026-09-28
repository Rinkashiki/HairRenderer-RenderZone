#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "frame_capture.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

void FrameCapture::init(Graphics::Device* device, uint32_t width, uint32_t height)
{
    m_device = device;
    m_width  = width;
    m_height = height;

    const size_t bufSize = static_cast<size_t>(width) * height * 4;
    m_stagingBuffer      = m_device->create_buffer_VMA(bufSize,
                                                  BUFFER_USAGE_TRANSFER_DST,
                                                  VMA_MEMORY_USAGE_GPU_TO_CPU);
    m_initialized = true;
}

void FrameCapture::cleanup()
{
    if (m_initialized)
    {
        m_stagingBuffer.cleanup();
        m_initialized = false;
    }
}

std::function<void(Graphics::Frame&, uint32_t)> FrameCapture::get_callback()
{
    return [this](Graphics::Frame& frame, uint32_t imageIndex) {
        Graphics::Image& img = m_device->get_swapchain().get_present_images()[imageIndex];

        // Swapchain image just finished being written by the final render pass —
        // transition from PRESENT_SRC to TRANSFER_SRC so we can copy it out.
        frame.commandBuffer.pipeline_barrier(img,
                                             LAYOUT_PRESENT,
                                             LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             ACCESS_COLOR_ATTACHMENT_WRITE,
                                             ACCESS_TRANSFER_READ,
                                             STAGE_COLOR_ATTACHMENT_OUTPUT,
                                             STAGE_TRANSFER);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent                 = {m_width, m_height, 1};

        vkCmdCopyImageToBuffer(frame.commandBuffer.handle,
                               img.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_stagingBuffer.handle,
                               1,
                               &region);

        // Transition back so the swapchain can present.
        frame.commandBuffer.pipeline_barrier(img,
                                             LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             LAYOUT_PRESENT,
                                             ACCESS_TRANSFER_READ,
                                             ACCESS_NONE,
                                             STAGE_TRANSFER,
                                             STAGE_BOTTOM_OF_PIPE);
    };
}

void FrameCapture::wait_and_write(uint32_t frameIndex, const std::filesystem::path& outDir)
{
    // Block until the GPU has finished writing the staging buffer.
    m_device->wait_queue(GRAPHIC_QUEUE);

    void* data = nullptr;
    VK_CHECK(vmaMapMemory(m_stagingBuffer.allocator, m_stagingBuffer.allocation, &data));

    // Swapchain format is B8G8R8A8_SRGB — swap B and R so stb writes correct RGB order.
    auto* px = reinterpret_cast<uint8_t*>(data);
    for (uint32_t i = 0; i < m_width * m_height; ++i)
        std::swap(px[i * 4 + 0], px[i * 4 + 2]);

    char name[32];
    std::snprintf(name, sizeof(name), "frame_%05u.png", frameIndex);
    const std::string path = (outDir / name).string();

    if (!stbi_write_png(path.c_str(), static_cast<int>(m_width), static_cast<int>(m_height), 4, data, static_cast<int>(m_width) * 4))
        throw std::runtime_error("stbi_write_png failed: " + path);

    vmaUnmapMemory(m_stagingBuffer.allocator, m_stagingBuffer.allocation);
}
