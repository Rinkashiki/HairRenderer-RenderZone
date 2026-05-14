#pragma once
#include <string>

namespace VKFW {
// Engine-wide resource root (defaults to the compile-time ENGINE_RESOURCES_PATH).
// Call set_engine_resources_path() before renderer init to override (e.g. SLViewer exe-relative path).
const std::string& get_engine_resources_path();
void               set_engine_resources_path(const std::string& path);
} // namespace VKFW
