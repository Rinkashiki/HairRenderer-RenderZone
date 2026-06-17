#pragma once

#include <engine/tools/gui.h>
#include <engine/tools/renderer_widget.h>
#include "hair_binding.h"
#include <vector>
USING_VULKAN_ENGINE_NAMESPACE

// Bind-mode panel: pick a strand-hair mesh, seat it on the head with transform
// sliders (live preview), then bind / save / load the surface binding. Operates
// on the HairBinder list owned by the application.
class HairBindWidget : public Tools::Widget {
    std::vector<hair_binding::HairBinder*>* m_binders{nullptr};
    int   m_active{0};
    float m_normalOffset{0.0f};
    bool  m_declip{true};

  protected:
    void render() override;

  public:
    HairBindWidget(std::vector<hair_binding::HairBinder*>* binders)
        : Tools::Widget({0.0f, 0.0f}, {0.0f, 0.0f})
        , m_binders(binders) {
    }
};

struct UserInterface {

    Tools::GUIOverlay*           overlay{nullptr};
    Tools::Panel*                explorer{nullptr};
    Tools::Panel*                properties{nullptr};
    Tools::SceneExplorerWidget*  sceneWidget{nullptr};
    Tools::ObjectExplorerWidget* objectWidget{nullptr};

    void init(Core::IWindow* window, Core::Scene* scene, Systems::BaseRenderer* renderer, bool* animateLight = nullptr);
};
