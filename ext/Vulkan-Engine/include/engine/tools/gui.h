/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License

    Copyright (c) 2023 Antonio Espinosa Garcia

*/
#ifndef GUI_H
#define GUI_H

#include <engine/graphics/device.h>
#include <engine/tools/widgets.h>

#include <functional>

// WIP..
// MUCH TO DO HERE, JUST READY FOR A SIMPLE DEMO
VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Tools
{

class GUIOverlay
{
  private:
    ImVec2 m_extent;

    std::vector<Panel *> m_panels;
    // Panel-less widgets drawn every frame before the panels (viewport tools:
    // gizmo, picking, floating toolbars). They own their ImGui windows, if any.
    std::vector<Widget *> m_viewportWidgets;
    // Optional app-provided style, applied every frame instead of the stock
    // colour profile (the profile is re-applied per frame, so a one-off
    // ImGui::GetStyle() edit would be overwritten).
    std::function<void()> m_styleCallback;

    GuiColorProfileType m_colorProfile;

    bool m_resized{false};

  public:
    GUIOverlay(float extentX, float extentY, GuiColorProfileType color = GuiColorProfileType::DARK)
        : m_extent({extentX, extentY}), m_colorProfile(color)
    {
    }
    ~GUIOverlay()
    {
        for (auto p : m_panels)
        {
            delete p;
        }
        for (auto w : m_viewportWidgets)
        {
            delete w;
        }
    }
    void render();
    inline void add_panel(Panel *p)
    {
        p->m_parentOverlay = this;
        m_panels.push_back(p);
    }
    inline void set_style_callback(std::function<void()> cb)
    {
        m_styleCallback = std::move(cb);
    }
    inline void add_viewport_widget(Widget *w)
    {
        m_viewportWidgets.push_back(w);
    }
    inline bool wants_to_handle_input() const
    {
        if (ImGui::GetCurrentContext())
        {
            ImGuiIO &io = ImGui::GetIO();
            if (io.WantCaptureMouse || io.WantCaptureKeyboard)
                return true;
            else
                return false;
        }
        return false;
    }

    inline math::vec2 get_extent() const
    {
        return {m_extent.x, m_extent.y};
    }
    inline void set_extent(math::vec2 p)
    {
        m_extent = {p.x, p.y};
        m_resized = true;
    }
};
} // namespace Tools

VULKAN_ENGINE_NAMESPACE_END

#endif