#include "resource_paths.h"

#include <engine/engine_config.h>

#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

std::string get_exe_dir() {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD len = GetModuleFileNameW(nullptr, buf, 4096);
    if (len == 0) return "./";
    std::filesystem::path p(std::wstring(buf, len));
    return p.parent_path().string() + "/";
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) return "./";
    buf[len] = '\0';
    std::filesystem::path p(buf);
    return p.parent_path().string() + "/";
#endif
}

std::string discover_resources_path() {
    std::string exeDir = get_exe_dir();

    // Case 1: deployed package — resources/ sits next to the binary
    std::string deployed = exeDir + "resources/";
    if (std::filesystem::exists(deployed)) {
        // In the deployed layout, engine resources (shaders, meshes, textures) share the same root.
        VKFW::set_engine_resources_path(deployed);
        return deployed;
    }

    // Case 2: dev build — binary is in build/, resources/ is one level up
    std::string dev = exeDir + "../resources/";
    if (std::filesystem::exists(dev)) {
        // Engine resources stay at their compile-time default (ENGINE_RESOURCES_PATH).
        return dev;
    }

    throw std::runtime_error(
        "Could not locate resources directory.\n"
        "Expected either:\n"
        "  " + deployed + "  (deployed layout)\n"
        "  " + std::filesystem::weakly_canonical(exeDir + "../resources/").string() + "  (dev layout)");
}
