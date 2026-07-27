#pragma once

#include <engine/tools/gui.h>
#include <engine/tools/renderer_widget.h>
#include <ImGuizmo.h>
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

// Transform gizmo (ImGuizmo) for the object currently picked in the Scene
// Explorer. Draws a full-screen manipulator over the 3D view (background draw
// list, so GUI panels stay on top) and writes the dragged transform back onto
// the selected Object3D. Operation/space are switchable in the panel and via
// hotkeys 1/2/3 (translate/rotate/scale) and X (world/local) — the camera owns
// W/E/R, so those are deliberately avoided.
class GizmoWidget : public Tools::Widget {
    Core::Scene*                m_scene{nullptr};
    Tools::SceneExplorerWidget* m_selection{nullptr};

    ImGuizmo::OPERATION m_op{ImGuizmo::TRANSLATE};
    ImGuizmo::MODE      m_mode{ImGuizmo::WORLD};
    // When true the gizmo is drawn at (and rotates/scales about) the selected
    // mesh's geometry-bounds center instead of its object origin — the assets
    // here often sit far from their origin. Non-destructive: the object's stored
    // transform/origin is never changed. Ignored for non-mesh selections.
    bool                m_pivotToGeometry{false};
    bool                m_useSnap{false};
    float               m_snapTranslate{0.1f};
    float               m_snapRotate{5.0f};   // degrees
    float               m_snapScale{0.1f};

  protected:
    void render() override;

  public:
    GizmoWidget(Core::Scene* scene, Tools::SceneExplorerWidget* selection)
        : Tools::Widget({0.0f, 0.0f}, {0.0f, 0.0f})
        , m_scene(scene)
        , m_selection(selection) {
    }
};

struct UserInterface {

    Tools::GUIOverlay*           overlay{nullptr};
    Tools::Panel*                explorer{nullptr};
    Tools::Panel*                properties{nullptr};
    Tools::SceneExplorerWidget*  sceneWidget{nullptr};
    Tools::ObjectExplorerWidget* objectWidget{nullptr};
    GizmoWidget*                 gizmoWidget{nullptr};

    void init(Core::IWindow* window, Core::Scene* scene, Systems::BaseRenderer* renderer, bool* animateLight = nullptr);
};
