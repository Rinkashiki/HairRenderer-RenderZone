/*
    This file is part of Vulkan-Engine, a simple to use Vulkan based 3D library

    MIT License

    Copyright (c) 2023 Antonio Espinosa Garcia

*/
#ifndef REND_WIDGET_H
#define REND_WIDGET_H

#include <engine/systems/renderers/deferred.h>
#include <engine/systems/renderers/forward.h>
#include <engine/systems/renderers/renderer.h>
#include <engine/tools/widgets.h>

VULKAN_ENGINE_NAMESPACE_BEGIN

namespace Tools {

class RendererSettingsWidget : public Widget
{

  protected:
    Systems::BaseRenderer* m_renderer;
    virtual void           render();

  public:
    RendererSettingsWidget(Systems::BaseRenderer* r)
        : Widget(ImVec2(0, 0), ImVec2(0, 0))
        , m_renderer(r) {
    }

    inline Systems::BaseRenderer* get_renderer() const {
        return m_renderer;
    }
    inline void set_renderer(Systems::BaseRenderer* r) {
        m_renderer = r;
    }
};
class DeferredRendererWidget : public Widget
{

  protected:
    Systems::DeferredRenderer* m_renderer;
    virtual void               render();

  public:
    DeferredRendererWidget(Systems::DeferredRenderer* r)
        : Widget(ImVec2(0, 0), ImVec2(0, 0))
        , m_renderer(r) {
    }

    inline Systems::DeferredRenderer* get_renderer() const {
        return m_renderer;
    }
    inline void set_renderer(Systems::DeferredRenderer* r) {
        m_renderer = r;
    }
};
class ForwardRendererWidget : public Widget
{

  protected:
    Systems::ForwardRenderer* m_renderer;
    bool*                     m_animateLight{nullptr};
    // UI-only split of the SSS scatter distance: the pass gets
    // normalize(m_scatterColor) * m_scatterDistance. Re-derived from the pass's
    // value whenever it changes from outside the panel (scene load, code).
    Vec3                      m_scatterColor{1.0f};
    float                     m_scatterDistance{0.0f};
    Vec3                      m_lastScatterVector{-1.0f};
    virtual void              render();

  public:
    ForwardRendererWidget(Systems::ForwardRenderer* r, bool* animateLight = nullptr)
        : Widget(ImVec2(0, 0), ImVec2(0, 0))
        , m_renderer(r)
        , m_animateLight(animateLight) {
    }

    inline Systems::ForwardRenderer* get_renderer() const {
        return m_renderer;
    }
    inline void set_renderer(Systems::ForwardRenderer* r) {
        m_renderer = r;
    }
};

} // namespace Tools

VULKAN_ENGINE_NAMESPACE_END
#endif
