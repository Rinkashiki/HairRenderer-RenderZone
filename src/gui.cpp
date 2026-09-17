#include "gui.h"
#include "app_info.h"
#include <cmath>
#include <cstdio>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/common.hpp>

// Union of every sub-geometry's bounds center, in the object's local space.
// Returns false when the object isn't a mesh or carries no geometry (lights,
// camera) — the caller then falls back to the object origin.
static bool mesh_geometry_center_local(Core::Object3D* obj, Vec3& outCenter) {
    if (!obj || obj->get_type() != ObjectType::MESH)
        return false;
    auto* mesh = static_cast<Core::Mesh*>(obj);
    Vec3  mn(INFINITY), mx(-INFINITY);
    bool  any = false;
    for (Core::Geometry* g : mesh->get_geometries()) {
        if (!g)
            continue;
        const auto& p = g->get_properties();
        mn            = glm::min(mn, p.minCoords);
        mx            = glm::max(mx, p.maxCoords);
        any           = true;
    }
    if (!any)
        return false;
    outCenter = (mn + mx) * 0.5f;
    return true;
}

void HairBindWidget::render() {
    ImGui::TextUnformatted("HAIR TO SCALP BINDING");
    ImGui::Separator();

    if (!m_binders || m_binders->empty()) {
        ImGui::TextDisabled("No strand hair / head mesh found.");
        return;
    }

    auto& binders = *m_binders;
    if (m_active >= (int)binders.size()) m_active = 0;

    // Hair selector.
    const std::string activeName = binders[m_active]->hair_mesh()->get_name();
    if (ImGui::BeginCombo("Hair", activeName.c_str())) {
        for (int i = 0; i < (int)binders.size(); ++i) {
            const std::string n = binders[i]->hair_mesh()->get_name();
            if (ImGui::Selectable(n.c_str(), i == m_active)) m_active = i;
        }
        ImGui::EndCombo();
    }

    hair_binding::HairBinder* binder = binders[m_active];
    Core::Mesh*               hair   = binder->hair_mesh();

    ImGui::Text("Head: %s", binder->head_mesh() ? binder->head_mesh()->get_name().c_str() : "<none>");
    ImGui::Text("Status: %s", binder->is_bound() ? "BOUND" : "unbound");
    ImGui::Spacing();

    // Gross alignment (edits the hair mesh's local transform; bind reads it).
    Vec3 p = hair->get_position();
    if (ImGui::DragFloat3("Position", glm::value_ptr(p), 0.01f)) hair->set_position(p);
    Vec3 r = hair->get_rotation();
    if (ImGui::DragFloat3("Rotation", glm::value_ptr(r), 0.5f)) hair->set_rotation(r);
    Vec3 s = hair->get_scale();
    if (ImGui::DragFloat3("Scale", glm::value_ptr(s), 0.01f)) hair->set_scale(s);

    ImGui::Spacing();
    ImGui::DragFloat("Normal offset", &m_normalOffset, 0.001f);
    ImGui::Checkbox("Declip to scalp", &m_declip);

    ImGui::Spacing();
    const std::string side = !binder->sidecar_path().empty() ? binder->sidecar_path()
                                                              : hair->get_file_route() + ".hbnd";
    if (ImGui::Button("Bind")) binder->bind(m_normalOffset, m_declip);
    ImGui::SameLine();
    if (ImGui::Button("Save")) binder->save(side);
    ImGui::SameLine();
    if (ImGui::Button("Load")) binder->load(side);
    ImGui::TextDisabled("%s", side.c_str());
}

void GizmoWidget::render() {
    ImGui::TextUnformatted("TRANSFORM GIZMO");
    ImGui::Separator();

    // Operation selector (hotkeys 1/2/3 — W/E/R belong to the camera).
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_1)) m_op = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) m_op = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) m_op = ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_X)) m_mode = (m_mode == ImGuizmo::WORLD) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }

    if (ImGui::RadioButton("Move (1)", m_op == ImGuizmo::TRANSLATE)) m_op = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (2)", m_op == ImGuizmo::ROTATE)) m_op = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale (3)", m_op == ImGuizmo::SCALE)) m_op = ImGuizmo::SCALE;

    // Space toggle. Scale is always local in ImGuizmo, so the control is moot there.
    ImGui::BeginDisabled(m_op == ImGuizmo::SCALE);
    if (ImGui::RadioButton("World", m_mode == ImGuizmo::WORLD)) m_mode = ImGuizmo::WORLD;
    ImGui::SameLine();
    if (ImGui::RadioButton("Local (X)", m_mode == ImGuizmo::LOCAL)) m_mode = ImGuizmo::LOCAL;
    ImGui::EndDisabled();

    // Pivot placement: object origin vs. geometry-bounds center (non-destructive).
    ImGui::Checkbox("Pivot at geometry center", &m_pivotToGeometry);

    ImGui::Checkbox("Snap", &m_useSnap);
    if (m_useSnap) {
        if (m_op == ImGuizmo::TRANSLATE) ImGui::DragFloat("Step", &m_snapTranslate, 0.01f, 0.0f, 0.0f, "%.3f");
        else if (m_op == ImGuizmo::ROTATE) ImGui::DragFloat("Step (deg)", &m_snapRotate, 0.1f, 0.0f, 0.0f, "%.2f");
        else ImGui::DragFloat("Step", &m_snapScale, 0.01f, 0.0f, 0.0f, "%.3f");
    }

    Core::Object3D* obj = m_selection ? m_selection->get_selected_object() : nullptr;
    if (!obj || !m_scene) {
        ImGui::TextDisabled("Select an object in the Explorer.");
        return;
    }
    ImGui::Text("Target: %s", obj->get_name().c_str());

    Core::Camera* cam = m_scene->get_active_camera();
    if (!cam)
        return;

    // Draw over the 3D view but under the ImGui panels.
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

    Mat4 view = cam->get_view();
    // The engine projection carries a Vulkan Y-flip (m_proj[1][1] *= -1) that
    // ImGuizmo doesn't expect; undo it on the copy we hand the gizmo, or the
    // manipulator draws mirrored and drags invert vertically.
    Mat4 proj = cam->get_projection();
    proj[1][1] *= -1.0f;

    // ImGuizmo works in world space. The object's real world matrix stays the
    // reference we transform; the gizmo we hand ImGuizmo shares that basis but
    // may be re-seated at the geometry center so it draws on the mesh and
    // rotates/scales about it. When the pivot is the origin, gizmo == objWorld
    // and this collapses to a plain absolute manipulation.
    Mat4 objWorld = obj->get_model_matrix();

    Mat4 gizmo = objWorld;
    Vec3 localCenter;
    if (m_pivotToGeometry && mesh_geometry_center_local(obj, localCenter)) {
        Vec3 worldCenter = Vec3(objWorld * Vec4(localCenter, 1.0f));
        gizmo[3]         = Vec4(worldCenter, 1.0f);
    }
    Mat4 gizmoManipulated = gizmo;

    float snap[3] = {0.0f, 0.0f, 0.0f};
    if (m_useSnap) {
        float s = (m_op == ImGuizmo::TRANSLATE) ? m_snapTranslate
                  : (m_op == ImGuizmo::ROTATE)  ? m_snapRotate
                                                : m_snapScale;
        snap[0] = snap[1] = snap[2] = s;
    }

    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view),
                                              glm::value_ptr(proj),
                                              m_op,
                                              m_mode,
                                              glm::value_ptr(gizmoManipulated),
                                              nullptr,
                                              m_useSnap ? snap : nullptr);

    if (changed) {
        // The world-space delta the gizmo underwent (about its pivot), applied
        // to the object's actual world matrix — so rotation/scale pivot about
        // the gizmo, not the object origin.
        Mat4 delta    = gizmoManipulated * glm::inverse(gizmo);
        Mat4 newWorld = delta * objWorld;

        Mat4 local = newWorld;
        if (Core::Object3D* parent = obj->get_parent())
            local = glm::inverse(parent->get_model_matrix()) * newWorld;

        float t[3], r[3], s[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(local), t, r, s);
        obj->set_position({t[0], t[1], t[2]});
        obj->set_rotation({r[0], r[1], r[2]}, false); // ImGuizmo returns degrees
        obj->set_scale({s[0], s[1], s[2]});
    }
}

void UserInterface::init(Core::IWindow* window, Core::Scene* scene, Systems::BaseRenderer* renderer, bool* animateLight) {

    overlay = new Tools::GUIOverlay(
        (float)window->get_extent().width, (float)window->get_extent().height, GuiColorProfileType::DARK);

    Tools::Panel* explorerPanel = new Tools::Panel("EXPLORER", 0, 0, 0.2f, 0.7f, PanelWidgetFlags::NoMove, false);
    sceneWidget                 = new Tools::SceneExplorerWidget(scene);
    explorerPanel->add_child(sceneWidget);
    explorerPanel->add_child(new Tools::Space());
    explorerPanel->add_child(new Tools::ForwardRendererWidget(static_cast<Systems::ForwardRenderer*>(renderer), animateLight));
    explorerPanel->add_child(new Tools::Separator());
    explorerPanel->add_child(new Tools::TextLine(" Application average"));
    explorerPanel->add_child(new Tools::Profiler());
    explorerPanel->add_child(new Tools::Space());

    overlay->add_panel(explorerPanel);
    explorer = explorerPanel;

    Tools::Panel* propertiesPanel =
        new Tools::Panel("OBJECT PROPERTIES", 0.75f, 0, 0.25f, 0.8f, PanelWidgetFlags::NoMove, true);
    gizmoWidget = new GizmoWidget(scene, sceneWidget);
    propertiesPanel->add_child(gizmoWidget);
    propertiesPanel->add_child(new Tools::Separator());
    objectWidget = new Tools::ObjectExplorerWidget();
    propertiesPanel->add_child(objectWidget);

    overlay->add_panel(propertiesPanel);
    properties = propertiesPanel;
}
// ─── Loading screen ──────────────────────────────────────────────────────────

namespace {
// Colours are authored in sRGB; the swapchain is an sRGB format and ImGui writes
// its vertex colours straight into it, so they get gamma-encoded a second time
// unless linearised first (a #0b0e13 backdrop would otherwise present as ~#3a4250).
ImU32 rgba(int r, int g, int b, float a) {
    auto lin = [](int c) { return (int)(std::pow((float)c / 255.0f, 2.2f) * 255.0f + 0.5f); };
    return IM_COL32(lin(r), lin(g), lin(b), (int)(a * 255.0f + 0.5f));
}
// Text width in the current font, with extra tracking between glyphs.
float tracked_text_width(const char* text, float tracking) {
    float w = 0.0f;
    for (const char* c = text; *c; ++c) {
        char one[2] = {*c, 0};
        w += ImGui::CalcTextSize(one).x + (c[1] ? tracking : 0.0f);
    }
    return w;
}
void add_tracked_text(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, float tracking) {
    for (const char* c = text; *c; ++c) {
        char one[2] = {*c, 0};
        dl->AddText(pos, col, one);
        pos.x += ImGui::CalcTextSize(one).x + tracking;
    }
}
} // namespace

void LoadingScreenWidget::render() {
    ImGuiIO&    io = ImGui::GetIO();
    const ImVec2 sz = io.DisplaySize;
    ImDrawList* dl  = ImGui::GetBackgroundDrawList();

    const double now = ImGui::GetTime();
    const float  dt  = m_lastTime < 0.0 ? 0.0f : (float)(now - m_lastTime);
    m_lastTime       = now;

    // Ease the drawn fill toward the reported fraction so per-image jumps read
    // as motion rather than steps. Never overshoots the target.
    const float target = m_progress ? m_progress->fraction() : 0.0f;
    m_shown += (target - m_shown) * (1.0f - std::exp(-dt * 6.0f));
    m_shown = std::min(m_shown, target);
    if (target >= 1.0f)
        m_shown = 1.0f; // last frame before the GPU upload blocks: show it full

    // Palette: near-black cool backdrop, ice-blue → mint accent.
    const ImU32 bgTop    = rgba(11, 14, 19, 1.0f);
    const ImU32 bgBottom = rgba(17, 22, 30, 1.0f);
    const ImU32 accentA  = rgba(88, 176, 255, 1.0f);
    const ImU32 accentB  = rgba(122, 232, 210, 1.0f);
    const ImU32 textHi   = rgba(236, 240, 245, 0.95f);
    const ImU32 textLo   = rgba(236, 240, 245, 0.42f);
    const ImU32 track    = rgba(255, 255, 255, 0.07f);

    dl->AddRectFilledMultiColor({0, 0}, sz, bgTop, bgTop, bgBottom, bgBottom);

    const ImVec2 center{sz.x * 0.5f, sz.y * 0.5f};

    // Soft glow behind the title block: stacked translucent discs, alpha rising
    // quadratically toward the centre so the outermost ones have no visible rim.
    // (Alpha blends in linear light before the sRGB encode, so it reads ~3x
    // stronger than the numbers suggest — keep them tiny.)
    {
        const float r0 = std::min(sz.x, sz.y) * 0.36f;
        const int   n  = 24;
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / (float)(n - 1);
            dl->AddCircleFilled({center.x, center.y - 20.0f}, r0 * (1.0f - t), rgba(70, 140, 220, 0.0045f * t * t), 64);
        }
    }

    // Title + subtitle.
    float y = center.y - 60.0f;
    {
        if (m_titleFont) ImGui::PushFont(m_titleFont);
        const std::string title    = app_info::name_upper();
        const float       tracking = 6.0f;
        const float       w        = tracked_text_width(title.c_str(), tracking);
        const float       h        = ImGui::GetFontSize();
        add_tracked_text(dl, {center.x - w * 0.5f, y - h}, textHi, title.c_str(), tracking);
        if (m_titleFont) ImGui::PopFont();
    }
    {
        if (m_bodyFont) ImGui::PushFont(m_bodyFont);
        const ImVec2 w = ImGui::CalcTextSize(m_subtitle.c_str());
        dl->AddText({center.x - w.x * 0.5f, y + 6.0f}, textLo, m_subtitle.c_str());
        if (m_bodyFont) ImGui::PopFont();
    }

    // Progress bar.
    const float  barW = std::min(440.0f, sz.x * 0.55f);
    const float  barH = 6.0f;
    const ImVec2 p0{center.x - barW * 0.5f, center.y + 52.0f};
    const ImVec2 p1{p0.x + barW, p0.y + barH};
    dl->AddRectFilled(p0, p1, track, barH * 0.5f);

    const float fillW = barW * std::max(m_shown, 0.0f);
    if (fillW > barH) {
        const ImVec2 f1{p0.x + fillW, p1.y};
        // Under-glow, then the gradient fill (clipped to the rounded track).
        dl->AddRectFilled({p0.x - 3.0f, p0.y - 5.0f}, {f1.x + 3.0f, f1.y + 5.0f}, rgba(88, 190, 255, 0.035f), barH);
        dl->PushClipRect(p0, f1, true);
        dl->AddRectFilled(p0, p1, accentA, barH * 0.5f);
        dl->AddRectFilledMultiColor(p0, f1, accentA, accentB, accentB, accentA);
        // Sheen sweeping left→right; keeps the bar alive while the target sits still
        // (e.g. the GPU upload stage), which is the visual "still working" cue.
        const float  period = 1.6f;
        const float  phase  = (float)std::fmod(now, (double)period) / period;
        const float  sheenW = 90.0f;
        const float  sx     = p0.x - sheenW + (fillW + 2.0f * sheenW) * phase;
        const ImU32  clear  = rgba(255, 255, 255, 0.0f);
        const ImU32  bright = rgba(255, 255, 255, 0.45f);
        dl->AddRectFilledMultiColor({sx, p0.y}, {sx + sheenW * 0.5f, p1.y}, clear, bright, bright, clear);
        dl->AddRectFilledMultiColor({sx + sheenW * 0.5f, p0.y}, {sx + sheenW, p1.y}, bright, clear, clear, bright);
        dl->PopClipRect();
    }

    // Stage line (left) and percentage (right), under the bar.
    {
        if (m_bodyFont) ImGui::PushFont(m_bodyFont);
        const std::string stage = m_progress ? m_progress->stage() : std::string();
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%d%%", (int)(m_shown * 100.0f + 0.5f));
        const float ty = p1.y + 14.0f;
        dl->AddText({p0.x, ty}, textLo, stage.c_str());
        const ImVec2 pw = ImGui::CalcTextSize(pct);
        dl->AddText({p1.x - pw.x, ty}, textHi, pct);
        if (m_bodyFont) ImGui::PopFont();
    }
}
