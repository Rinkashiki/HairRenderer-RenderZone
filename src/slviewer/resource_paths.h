#pragma once
#include <string>

// Returns the absolute directory of the running executable, with a trailing slash.
// Falls back to "./" on error.
std::string get_exe_dir();

// Probes for the resource root in this order:
//   1. <exe_dir>/resources/           — deployed package layout
//   2. <exe_dir>/../resources/        — dev build (binary in build/ one level below project root)
// Returns the first path that exists (with trailing slash), or throws std::runtime_error
// if neither is found.
//
// Also calls VKFW::set_engine_resources_path() with the discovered path when we are in the
// deployed layout (case 1), so shaders/meshes/textures resolve from the same root.
// In the dev layout (case 2) the engine keeps its compile-time default.
std::string discover_resources_path();
