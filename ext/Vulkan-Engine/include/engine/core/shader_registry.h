#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace VKFW {

// One pre-compiled SPIR-V blob, one engine shader stage. The `path` is
// engine-resource-relative (e.g. "shaders/forward/phong.glsl") — the runtime
// strips the engine-resources prefix from incoming filePaths before lookup.
struct EmbeddedShaderEntry {
    const char*           path;
    VkShaderStageFlagBits stage;
    const uint32_t*       code;
    std::size_t           codeWordCount;
};

// SLViewer's generated embedded_shaders.cpp installs the registry at static
// init. HairViewer never registers one and continues to use Shaderc at runtime.
void set_embedded_shader_registry(const EmbeddedShaderEntry* entries,
                                  std::size_t                count);

bool has_embedded_shader_registry();

// Returns every entry whose `path` matches `filePath` (after stripping the
// engine-resources prefix). Empty when no registry is installed or no match.
std::vector<const EmbeddedShaderEntry*>
find_embedded_shaders_for_file(const std::string& filePath);

} // namespace VKFW
