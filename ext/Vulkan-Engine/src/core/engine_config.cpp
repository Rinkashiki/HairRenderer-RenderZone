#include <engine/engine_config.h>

namespace VKFW {

static std::string g_engineResourcesPath = ENGINE_RESOURCES_PATH;

const std::string& get_engine_resources_path() { return g_engineResourcesPath; }
void               set_engine_resources_path(const std::string& path) { g_engineResourcesPath = path; }

} // namespace VKFW
