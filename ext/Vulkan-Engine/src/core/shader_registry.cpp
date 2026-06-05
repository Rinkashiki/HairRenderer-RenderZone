#include <engine/core/shader_registry.h>
#include <engine/engine_config.h>

#include <algorithm>

namespace VKFW {

static const EmbeddedShaderEntry* g_entries = nullptr;
static std::size_t                g_count   = 0;

void set_embedded_shader_registry(const EmbeddedShaderEntry* entries,
                                  std::size_t                count) {
    g_entries = entries;
    g_count   = count;
}

bool has_embedded_shader_registry() {
    return g_entries != nullptr && g_count > 0;
}

std::vector<const EmbeddedShaderEntry*>
find_embedded_shaders_for_file(const std::string& filePath) {
    std::vector<const EmbeddedShaderEntry*> hits;
    if (!has_embedded_shader_registry())
        return hits;

    std::string key = filePath;

    // Strip engine-resources prefix so the key matches the relative paths
    // baked into the registry (e.g. "shaders/forward/phong.glsl").
    const std::string& engineRoot = get_engine_resources_path();
    if (!engineRoot.empty() && key.rfind(engineRoot, 0) == 0)
        key = key.substr(engineRoot.length());

    // Normalize backslashes on Windows so lookups match the forward-slash keys.
    std::replace(key.begin(), key.end(), '\\', '/');

    for (std::size_t i = 0; i < g_count; ++i)
    {
        if (key == g_entries[i].path)
            hits.push_back(&g_entries[i]);
    }
    return hits;
}

} // namespace VKFW
