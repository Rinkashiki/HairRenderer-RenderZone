#pragma once
// Interface themes for the ZoneRenderer GUI.
//
// Applied every frame through GUIOverlay::set_style_callback (the overlay used
// to re-apply ImGui's stock dark profile each frame). Colours are authored in
// sRGB and linearised, because the swapchain is sRGB and ImGui writes vertex
// colours straight into it (same reason as the loading screen's rgba()).
// Panels push their own rounding/padding/border every frame, so the theme
// restyles registered panels too.

#include <vector>

#include <engine/tools/widgets.h>

namespace gui_theme {

enum class Theme {
    Slate    = 0, // matches the loading screen: cool near-black, ice-blue -> mint
    Filament = 1, // warm espresso/charcoal, copper -> amber
    Stock    = 2, // ImGui's default dark theme (the previous look)
    Count
};

const char* name(Theme t);

Theme current();
void  set_current(Theme t);

// UI body font (Roboto Medium) used by the custom themes; Stock keeps ImGui's.
void set_ui_font(ImFont* font);
// Panels whose per-frame style pushes (rounding, padding, border) follow the theme.
void register_panel(VKFW::Tools::Panel* panel);

// Apply current() to ImGui::GetStyle(), the default font and registered panels.
void apply();

// Theme-aware accent for app widgets (e.g. warnings in the viewport toolbar).
ImVec4 warning_color();

} // namespace gui_theme
