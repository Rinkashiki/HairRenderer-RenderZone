#include "gui_theme.h"

#include <cmath>

namespace gui_theme {
namespace {

Theme                            s_current = Theme::Slate;
ImFont*                          s_uiFont  = nullptr;
std::vector<VKFW::Tools::Panel*> s_panels;

// sRGB hex (0xRRGGBB) -> linear ImVec4 (see header).
ImVec4 hex(unsigned rgb, float a = 1.0f) {
    auto lin = [](unsigned c) { return std::pow((float)c / 255.0f, 2.2f); };
    return ImVec4(lin((rgb >> 16) & 0xFF), lin((rgb >> 8) & 0xFF), lin(rgb & 0xFF), a);
}

// A theme is a handful of roles; every ImGuiCol is derived from them so both
// themes stay consistent with each other.
struct Palette {
    unsigned bg;          // window background
    float    bgAlpha;     // panels float over the render
    unsigned panel;       // popups, title bars, table headers
    unsigned frame;       // input/slider/checkbox background
    unsigned frameHover;
    unsigned frameActive;
    unsigned border;
    unsigned text;
    unsigned textDim;
    unsigned accent;      // primary: slider grabs, focus, links
    unsigned accentAlt;   // secondary: check marks, active grabs
    unsigned accentDeep;  // pressed/"on" buttons, active headers (text stays readable)
    unsigned selection;   // selected rows (tree/selectable)
    float    rounding;
};

const Palette kSlate{
    0x0B0E13, 0.94f, 0x11161E, 0x161C26, 0x1D2531, 0x243042, 0x1E2633,
    0xECF0F5, 0x6B7686, 0x58B0FF, 0x7AE8D2, 0x235A8C, 0x1C3A5A, 6.0f,
};

const Palette kFilament{
    0x16120F, 0.94f, 0x1F1915, 0x241D18, 0x2E251E, 0x3A2E24, 0x33291F,
    0xEDE3D6, 0x8F8172, 0xD98A4E, 0xF0B27A, 0x7A4A26, 0x4A3020, 5.0f,
};

void apply_palette(const Palette& p) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec4*     c  = st.Colors;

    c[ImGuiCol_Text]                  = hex(p.text);
    c[ImGuiCol_TextDisabled]          = hex(p.textDim);
    c[ImGuiCol_WindowBg]              = hex(p.bg, p.bgAlpha);
    c[ImGuiCol_ChildBg]               = hex(p.bg, 0.0f);
    c[ImGuiCol_PopupBg]               = hex(p.panel, 0.98f);
    c[ImGuiCol_Border]                = hex(p.border);
    c[ImGuiCol_BorderShadow]          = hex(0x000000, 0.0f);
    c[ImGuiCol_FrameBg]               = hex(p.frame);
    c[ImGuiCol_FrameBgHovered]        = hex(p.frameHover);
    c[ImGuiCol_FrameBgActive]         = hex(p.frameActive);
    c[ImGuiCol_TitleBg]               = hex(p.bg, p.bgAlpha);
    c[ImGuiCol_TitleBgActive]         = hex(p.panel, p.bgAlpha);
    c[ImGuiCol_TitleBgCollapsed]      = hex(p.bg, 0.80f);
    c[ImGuiCol_MenuBarBg]             = hex(p.panel);
    c[ImGuiCol_ScrollbarBg]           = hex(p.bg, 0.0f);
    c[ImGuiCol_ScrollbarGrab]         = hex(p.frameActive);
    c[ImGuiCol_ScrollbarGrabHovered]  = hex(p.accentDeep);
    c[ImGuiCol_ScrollbarGrabActive]   = hex(p.accent);
    c[ImGuiCol_CheckMark]             = hex(p.accentAlt);
    c[ImGuiCol_SliderGrab]            = hex(p.accent);
    c[ImGuiCol_SliderGrabActive]      = hex(p.accentAlt);
    c[ImGuiCol_Button]                = hex(p.frame);
    c[ImGuiCol_ButtonHovered]         = hex(p.frameActive);
    c[ImGuiCol_ButtonActive]          = hex(p.accentDeep);
    c[ImGuiCol_Header]                = hex(p.selection);
    c[ImGuiCol_HeaderHovered]         = hex(p.frameHover);
    c[ImGuiCol_HeaderActive]          = hex(p.accentDeep);
    c[ImGuiCol_Separator]             = hex(p.border);
    c[ImGuiCol_SeparatorHovered]      = hex(p.accent);
    c[ImGuiCol_SeparatorActive]       = hex(p.accentAlt);
    c[ImGuiCol_ResizeGrip]            = hex(p.accent, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = hex(p.accent, 0.60f);
    c[ImGuiCol_ResizeGripActive]      = hex(p.accentAlt, 0.90f);
    c[ImGuiCol_Tab]                   = hex(p.frame);
    c[ImGuiCol_TabHovered]            = hex(p.accentDeep);
    c[ImGuiCol_TabActive]             = hex(p.selection);
    c[ImGuiCol_TabUnfocused]          = hex(p.frame);
    c[ImGuiCol_TabUnfocusedActive]    = hex(p.selection);
    c[ImGuiCol_PlotLines]             = hex(p.accent);
    c[ImGuiCol_PlotLinesHovered]      = hex(p.accentAlt);
    c[ImGuiCol_PlotHistogram]         = hex(p.accent);
    c[ImGuiCol_PlotHistogramHovered]  = hex(p.accentAlt);
    c[ImGuiCol_TableHeaderBg]         = hex(p.panel);
    c[ImGuiCol_TableBorderStrong]     = hex(p.border);
    c[ImGuiCol_TableBorderLight]      = hex(p.frame);
    c[ImGuiCol_TableRowBg]            = hex(0x000000, 0.0f);
    c[ImGuiCol_TableRowBgAlt]         = hex(0xFFFFFF, 0.025f);
    c[ImGuiCol_TextSelectedBg]        = hex(p.accent, 0.35f);
    c[ImGuiCol_DragDropTarget]        = hex(p.accentAlt);
    c[ImGuiCol_NavHighlight]          = hex(p.accent);
    c[ImGuiCol_NavWindowingHighlight] = hex(0xFFFFFF, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = hex(0x000000, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = hex(0x000000, 0.45f);

    // Shape and spacing: roomier than stock, soft corners, hairline borders.
    st.WindowRounding           = p.rounding + 2.0f;
    st.ChildRounding            = p.rounding;
    st.FrameRounding            = p.rounding;
    st.PopupRounding            = p.rounding;
    st.ScrollbarRounding        = p.rounding;
    st.GrabRounding             = p.rounding;
    st.TabRounding              = p.rounding;
    st.WindowBorderSize         = 1.0f;
    st.ChildBorderSize          = 1.0f;
    st.PopupBorderSize          = 1.0f;
    st.FrameBorderSize          = 0.0f;
    st.WindowPadding            = ImVec2(10.0f, 8.0f);
    st.FramePadding             = ImVec2(8.0f, 4.0f);
    st.ItemSpacing              = ImVec2(8.0f, 6.0f);
    st.ItemInnerSpacing         = ImVec2(6.0f, 4.0f);
    st.CellPadding              = ImVec2(6.0f, 3.0f);
    st.IndentSpacing            = 16.0f;
    st.ScrollbarSize            = 10.0f;
    st.GrabMinSize              = 10.0f;
    st.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    st.SeparatorTextBorderSize  = 1.0f;
    st.SeparatorTextPadding     = ImVec2(8.0f, 4.0f);
    st.SeparatorTextAlign       = ImVec2(0.0f, 0.5f);

    ImGui::GetIO().FontDefault = s_uiFont;

    for (auto* panel : s_panels) {
        panel->set_rounding(p.rounding);
        panel->set_padding({10.0f, 8.0f});
        panel->set_border_size(1.0f);
    }
}

void apply_stock() {
    ImGuiStyle& st = ImGui::GetStyle();
    st             = ImGuiStyle(); // metrics back to ImGui defaults
    ImGui::StyleColorsDark();
    ImGui::GetIO().FontDefault = nullptr;
    // The engine's Panel defaults (see widgets.h).
    for (auto* panel : s_panels) {
        panel->set_rounding(12.0f);
        panel->set_padding({8.0f, 5.0f});
        panel->set_border_size(0.0f);
    }
}

} // namespace

const char* name(Theme t) {
    switch (t) {
    case Theme::Slate: return "Slate (matches loading screen)";
    case Theme::Filament: return "Warm filament";
    case Theme::Stock: return "ImGui default";
    default: return "?";
    }
}

Theme current() {
    return s_current;
}
void set_current(Theme t) {
    s_current = t;
}
void set_ui_font(ImFont* font) {
    s_uiFont = font;
}
void register_panel(VKFW::Tools::Panel* panel) {
    if (panel)
        s_panels.push_back(panel);
}

void apply() {
    switch (s_current) {
    case Theme::Slate: apply_palette(kSlate); break;
    case Theme::Filament: apply_palette(kFilament); break;
    default: apply_stock(); break;
    }
}

ImVec4 warning_color() {
    switch (s_current) {
    case Theme::Slate: return hex(0xFFC46B);
    case Theme::Filament: return hex(0xF0B27A);
    default: return ImVec4(1.0f, 0.75f, 0.35f, 1.0f);
    }
}

} // namespace gui_theme
