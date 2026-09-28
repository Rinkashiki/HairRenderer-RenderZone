#pragma once

#include <engine/tools/gui.h>
#include <engine/tools/controller.h>
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

// What the editor lets you change on an object — the unit of undo/redo.
struct TransformState {
    Core::Object3D* parent{nullptr};
    Vec3            position{0.0f};
    Vec3            rotation{0.0f}; // degrees, engine Euler order
    Vec3            scale{1.0f};
    bool            hasDirection{false}; // directional lights: aim
    Vec3            direction{0.0f};

    static TransformState capture(Core::Object3D* obj);
    void                  apply(Core::Object3D* obj) const;
    bool                  same_as(const TransformState& o) const;
};

// Bounded undo/redo stack of transform edits (gizmo drags, Properties fields,
// binding sliders — anything that changes the selected object's transform while
// the user is interacting).
class TransformHistory {
  public:
    struct Entry {
        Core::Object3D* object{nullptr};
        TransformState  before;
        TransformState  after;
    };

    void push(const Entry& e) {
        m_undo.push_back(e);
        if (m_undo.size() > kMaxEntries)
            m_undo.erase(m_undo.begin());
        m_redo.clear();
    }
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    // Returns the entry that was undone/redone (object == nullptr if none).
    Entry undo();
    Entry redo();

  private:
    static constexpr size_t kMaxEntries = 128;
    std::vector<Entry>      m_undo;
    std::vector<Entry>      m_redo;
};

// Everything that lives *in* the 3D view, drawn every frame by the overlay
// (not inside a panel, so it works whatever panels are open or collapsed):
//  - floating toolbar (top centre) for the selection: Move/Rotate/Scale,
//    World/Local, pivot, snap, focus, undo/redo, outline settings;
//  - the ImGuizmo transform gizmo on the selected object;
//  - click-to-select (CPU raycast, see picking.h) and click-away to deselect.
// Selection itself lives in the Scene Explorer widget (single source of truth).
//
// Hotkeys (only while no text field has focus): 1/2/3 move/rotate/scale, X
// world/local, F focus, Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) undo/redo, hold Ctrl
// while dragging to invert Snap. W/A/S/D/Q/E/R stay with the camera.
//
// Per-type rules: meshes get every operation; point/spot lights only move;
// a directional light moves (its shadow/dummy position) and rotates, where
// rotation aims its direction; the camera has no gizmo. A light's marker mesh
// manipulates the light.
class ViewportWidget : public Tools::Widget {
    Core::Scene*                            m_scene{nullptr};
    Tools::SceneExplorerWidget*             m_selection{nullptr};
    Tools::Controller*                      m_controller{nullptr};
    std::vector<hair_binding::HairBinder*>* m_binders{nullptr};
    Systems::ForwardRenderer*               m_renderer{nullptr};

    ImGuizmo::OPERATION m_op{ImGuizmo::TRANSLATE};
    ImGuizmo::MODE      m_mode{ImGuizmo::WORLD};
    // Gizmo drawn at (and rotating/scaling about) the mesh's geometry-bounds
    // center instead of its object origin — assets here often sit far from their
    // origin. Non-destructive: the stored transform/origin never changes.
    // Default on: with the pivot at the origin the gizmo is often off-screen.
    bool  m_pivotToGeometry{true};
    bool  m_useSnap{false};
    float m_snapTranslate{0.1f};
    float m_snapRotate{5.0f}; // degrees
    float m_snapScale{0.1f};

    float m_toolbarWidth{600.0f}; // last frame's, for placement

    // Picking
    float m_pickTolerancePx{4.0f}; // strand hair hit radius
    bool  m_pressInViewport{false};

    // Undo: watch the selected object and record a change once the user
    // interaction that caused it ends.
    TransformHistory m_history;
    Core::Object3D*  m_watched{nullptr};
    TransformState   m_stable;
    bool             m_touched{false};

    // Directional-light gizmo frame, kept for the length of a drag so the rings
    // don't re-roll every frame (the light itself only stores a direction).
    Mat3 m_lightFrame{1.0f};
    bool m_lightFrameLive{false};

    Core::Object3D* gizmo_target(Core::Object3D* sel) const;
    void            draw_toolbar(Core::Object3D* sel, Core::Object3D* target);
    void            manipulate(Core::Object3D* target);
    void            draw_light_direction(Core::Object3D* target);
    void            watch_for_edits(Core::Object3D* target);
    void            handle_click();
    void            focus(Core::Object3D* target);
    void            apply_history(const TransformHistory::Entry& e, bool undo);
    bool            op_allowed(Core::Object3D* target, ImGuizmo::OPERATION op) const;

  protected:
    void render() override;

  public:
    ViewportWidget(Core::Scene*                            scene,
                   Tools::SceneExplorerWidget*             selection,
                   Tools::Controller*                      controller,
                   std::vector<hair_binding::HairBinder*>* binders,
                   Systems::ForwardRenderer*               renderer)
        : Tools::Widget({0.0f, 0.0f}, {0.0f, 0.0f})
        , m_scene(scene)
        , m_selection(selection)
        , m_controller(controller)
        , m_binders(binders)
        , m_renderer(renderer) {
    }
};

struct UserInterface {

    Tools::GUIOverlay*           overlay{nullptr};
    Tools::Panel*                explorer{nullptr};
    Tools::Panel*                properties{nullptr};
    Tools::SceneExplorerWidget*  sceneWidget{nullptr};
    Tools::ObjectExplorerWidget* objectWidget{nullptr};
    ViewportWidget*              viewport{nullptr};

    void init(Core::IWindow*                          window,
              Core::Scene*                            scene,
              Systems::BaseRenderer*                  renderer,
              Tools::Controller*                      controller,
              std::vector<hair_binding::HairBinder*>* binders,
              bool*                                   animateLight = nullptr);
};
