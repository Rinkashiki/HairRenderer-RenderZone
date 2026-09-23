#pragma once

// User-facing identity of the interactive app. The name is spelled exactly once,
// in CMakeLists.txt (APP_DISPLAY_NAME); everything that shows it — window title,
// loading screen, ... — reads it from here. Never write the name as a literal.
#ifndef APP_DISPLAY_NAME
#error "APP_DISPLAY_NAME must be defined by the build (see CMakeLists.txt)"
#endif

#include <cctype>
#include <string>

namespace app_info {

constexpr const char* NAME = APP_DISPLAY_NAME;

// "Zone Renderer" -> "ZONE RENDERER", for display styles that want caps.
inline std::string name_upper() {
    std::string s = NAME;
    for (char& c : s)
        c = (char)std::toupper((unsigned char)c);
    return s;
}

} // namespace app_info
