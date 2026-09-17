#pragma once

#include <engine/tools/gui.h>
#include <engine/tools/renderer_widget.h>
#include <ImGuizmo.h>
#include "hair_binding.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
USING_VULKAN_ENGINE_NAMESPACE

// Progress shared between the scene-loader worker and the loading screen
// drawn on the main thread. Fraction is monotonic; the stage line is whatever
// the loader is doing right now.
struct LoadingProgress {
    void set(float fraction, const std::string& stage) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_fraction = std::max(m_fraction.load(), fraction);
        m_stage    = stage;
    }
    float       fraction() const { return m_fraction.load(); }
    std::string stage() {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_stage;
    }

  private:
    std::atomic<float> m_fraction{0.0f};
    std::mutex         m_mtx;
    std::string        m_stage;
};

// Full-screen loading screen: dark gradient backdrop, title, a rounded
// progress bar with an eased fill and a moving sheen, and the loader's current
// stage line. Draws straight to ImGui's background draw list, so the Panel
// hosting it only needs to exist (it is made invisible). Fonts are optional —
// null falls back to ImGui's default.
class LoadingScreenWidget : public Tools::Widget {
    LoadingProgress* m_progress{nullptr};
    ImFont*          m_titleFont{nullptr};
    ImFont*          m_bodyFont{nullptr};
    std::string      m_subtitle;
    float            m_shown{0.0f}; // eased fraction actually drawn
    double           m_lastTime{-1.0};

  protected:
    void render() override;

  public:
    LoadingScreenWidget(LoadingProgress* progress, ImFont* titleFont, ImFont* bodyFont, std::string subtitle)
        : Tools::Widget({0.0f, 0.0f}, {0.0f, 0.0f})
        , m_progress(progress)
        , m_titleFont(titleFont)
        , m_bodyFont(bodyFont)
        , m_subtitle(std::move(subtitle)) {
    }
};

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
