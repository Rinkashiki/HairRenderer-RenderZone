#include "gui.h"
#include "app_info.h"
#include "gui_theme.h"
#include "picking.h"
#include "transform_utils.h"
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

// ─── Transform state / history ───────────────────────────────────────────────

TransformState TransformState::capture(Core::Object3D* obj) {
    TransformState st;
    st.parent   = obj->get_parent();
    st.position = obj->get_position();
    st.rotation = obj->get_rotation();
    st.scale    = obj->get_scale();
    if (obj->get_type() == ObjectType::LIGHT &&
        static_cast<Core::Light*>(obj)->get_light_type() == LightType::DIRECTIONAL) {
        st.hasDirection = true;
        st.direction    = static_cast<Core::DirectionalLight*>(obj)->get_direction();
    }
    return st;
}

void TransformState::apply(Core::Object3D* obj) const {
    obj->set_position(position);
    obj->set_rotation(rotation);
    obj->set_scale(scale);
    if (hasDirection && obj->get_type() == ObjectType::LIGHT &&
        static_cast<Core::Light*>(obj)->get_light_type() == LightType::DIRECTIONAL)
        static_cast<Core::DirectionalLight*>(obj)->set_direction(direction);
}

bool TransformState::same_as(const TransformState& o) const {
    auto eq = [](const Vec3& a, const Vec3& b) { return glm::all(glm::lessThanEqual(glm::abs(a - b), Vec3(1e-5f))); };
    return parent == o.parent && eq(position, o.position) && eq(rotation, o.rotation) && eq(scale, o.scale) &&
           hasDirection == o.hasDirection && (!hasDirection || eq(direction, o.direction));
}

TransformHistory::Entry TransformHistory::undo() {
    if (m_undo.empty())
        return {};
    Entry e = m_undo.back();
    m_undo.pop_back();
    m_redo.push_back(e);
    return e;
}

TransformHistory::Entry TransformHistory::redo() {
    if (m_redo.empty())
        return {};
    Entry e = m_redo.back();
    m_redo.pop_back();
    m_undo.push_back(e);
    return e;
}

// ─── Viewport widget ─────────────────────────────────────────────────────────

namespace {

bool is_light(Core::Object3D* o, LightType t) {
    return o && o->get_type() == ObjectType::LIGHT && static_cast<Core::Light*>(o)->get_light_type() == t;
}

// Button that reads as "pressed" while `on`. Hover tooltip also shows when disabled.
bool toggle_button(const char* label, bool on, bool enabled = true, const char* tooltip = nullptr) {
    ImGui::BeginDisabled(!enabled);
    if (on)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = ImGui::Button(label);
    if (on)
        ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

// World point -> ImGui display coordinates, using the engine's Vulkan-style
// projection (y-down NDC). False if behind the camera.
bool project_to_screen(Core::Camera* cam, const Vec3& p, ImVec2& out) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    Vec4         c    = cam->get_projection() * cam->get_view() * Vec4(p, 1.0f);
    if (c.w <= 1e-5f)
        return false;
    out = ImVec2((c.x / c.w * 0.5f + 0.5f) * size.x, (c.y / c.w * 0.5f + 0.5f) * size.y);
    return true;
}

// Orthonormal frame whose +Z is `dir` (roll is arbitrary but stable).
Mat3 frame_from_direction(const Vec3& dir) {
    const Vec3 z  = glm::normalize(dir);
    const Vec3 up = std::abs(z.y) > 0.99f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    const Vec3 x  = glm::normalize(glm::cross(up, z));
    const Vec3 y  = glm::cross(z, x);
    return Mat3(x, y, z);
}

// World-space AABB of a mesh (union of its geometries' local bounds).
bool mesh_world_bounds(Core::Object3D* obj, Vec3& outMin, Vec3& outMax) {
    if (!obj || obj->get_type() != ObjectType::MESH)
        return false;
    auto* mesh = static_cast<Core::Mesh*>(obj);
    Vec3  mn(INFINITY), mx(-INFINITY);
    bool  any = false;
    for (Core::Geometry* g : mesh->get_geometries()) {
        if (!g)
            continue;
        mn  = glm::min(mn, g->get_properties().minCoords);
        mx  = glm::max(mx, g->get_properties().maxCoords);
        any = true;
    }
    if (!any)
        return false;
    const Mat4 m = obj->get_model_matrix();
    outMin       = Vec3(INFINITY);
    outMax       = Vec3(-INFINITY);
    for (int i = 0; i < 8; ++i) {
        Vec3 c((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
        Vec3 w = Vec3(m * Vec4(c, 1.0f));
        outMin = glm::min(outMin, w);
        outMax = glm::max(outMax, w);
    }
    return true;
}

} // namespace

Core::Object3D* ViewportWidget::gizmo_target(Core::Object3D* sel) const {
    if (!sel)
        return nullptr;
    if (sel->get_type() == ObjectType::CAMERA)
        return nullptr;
    // A light's marker mesh stands in for the light.
    Core::Object3D* parent = sel->get_parent();
    if (sel->get_type() == ObjectType::MESH && parent && parent->get_type() == ObjectType::LIGHT)
        return parent;
    return sel;
}

bool ViewportWidget::op_allowed(Core::Object3D* target, ImGuizmo::OPERATION op) const {
    if (!target)
        return false;
    if (target->get_type() == ObjectType::LIGHT) {
        if (op == ImGuizmo::TRANSLATE)
            return true;
        return op == ImGuizmo::ROTATE && is_light(target, LightType::DIRECTIONAL);
    }
    return true;
}

void ViewportWidget::render() {
    ImGuiIO& io = ImGui::GetIO();
    if (!m_scene || !m_selection)
        return;

    Core::Object3D* sel    = m_selection->get_selected_object();
    Core::Object3D* target = gizmo_target(sel);

    // Hotkeys — never while typing into a field, and never mid-drag.
    if (!io.WantCaptureKeyboard && !ImGuizmo::IsUsing()) {
        if (io.KeyCtrl) {
            const bool redo = ImGui::IsKeyPressed(ImGuiKey_Y, false) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false));
            if (redo)
                apply_history(m_history.redo(), false);
            else if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
                apply_history(m_history.undo(), true);
        } else {
            if (ImGui::IsKeyPressed(ImGuiKey_1, false)) m_op = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_2, false)) m_op = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_3, false)) m_op = ImGuizmo::SCALE;
            if (ImGui::IsKeyPressed(ImGuiKey_X, false)) m_mode = (m_mode == ImGuizmo::WORLD) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focus(target);
        }
        // Undo may have changed the selection.
        sel    = m_selection->get_selected_object();
        target = gizmo_target(sel);
    }

    if (sel)
        draw_toolbar(sel, target);
    if (target) {
        manipulate(target);
        draw_light_direction(target);
    }
    watch_for_edits(target);
    handle_click();
}

void ViewportWidget::draw_toolbar(Core::Object3D* sel, Core::Object3D* target) {
    ImGuiIO& io = ImGui::GetIO();
    // Centred in the strip between the EXPLORER (left 20%) and OBJECT PROPERTIES
    // (right 25%) panels, just below their title bars; never over the explorer.
    // Uses last frame's width (auto-resized window).
    const float left = io.DisplaySize.x * 0.2f + 8.0f;
    float       x    = io.DisplaySize.x * 0.475f - m_toolbarWidth * 0.5f;
    x                = std::max(left, std::min(x, io.DisplaySize.x - m_toolbarWidth - 8.0f));
    ImGui::SetNextWindowPos(ImVec2(x, 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("##viewport_toolbar", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // Operation.
    const bool canMove   = op_allowed(target, ImGuizmo::TRANSLATE);
    const bool canRotate = op_allowed(target, ImGuizmo::ROTATE);
    const bool canScale  = op_allowed(target, ImGuizmo::SCALE);
    if (!op_allowed(target, m_op) && canMove)
        m_op = ImGuizmo::TRANSLATE;
    if (toggle_button("Move", m_op == ImGuizmo::TRANSLATE, canMove, "Move (1)")) m_op = ImGuizmo::TRANSLATE;
    ImGui::SameLine(0, 2);
    if (toggle_button("Rotate", m_op == ImGuizmo::ROTATE, canRotate,
                      is_light(target, LightType::DIRECTIONAL) ? "Rotate (2) - aims the light" : "Rotate (2)"))
        m_op = ImGuizmo::ROTATE;
    ImGui::SameLine(0, 2);
    if (toggle_button("Scale", m_op == ImGuizmo::SCALE, canScale, "Scale (3)")) m_op = ImGuizmo::SCALE;

    // Space (ImGuizmo always scales in local space).
    ImGui::SameLine(0, 12);
    const bool spaceMatters = target && m_op != ImGuizmo::SCALE;
    if (toggle_button("World", m_mode == ImGuizmo::WORLD, spaceMatters, "Gizmo axes: world (X toggles)")) m_mode = ImGuizmo::WORLD;
    ImGui::SameLine(0, 2);
    if (toggle_button("Local", m_mode == ImGuizmo::LOCAL, spaceMatters, "Gizmo axes: object (X toggles)")) m_mode = ImGuizmo::LOCAL;

    // Pivot (meshes only).
    ImGui::SameLine(0, 12);
    const bool isMesh = target && target->get_type() == ObjectType::MESH;
    if (toggle_button("Origin", !m_pivotToGeometry, isMesh, "Pivot at the object origin")) m_pivotToGeometry = false;
    ImGui::SameLine(0, 2);
    if (toggle_button("Center", m_pivotToGeometry, isMesh,
                      "Pivot at the geometry-bounds center (non-destructive: the origin is not moved)"))
        m_pivotToGeometry = true;

    // Snap.
    ImGui::SameLine(0, 12);
    ImGui::Checkbox("Snap", &m_useSnap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap while dragging. Hold Ctrl to invert.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    if (m_op == ImGuizmo::TRANSLATE) ImGui::DragFloat("##snapT", &m_snapTranslate, 0.01f, 0.001f, 100.0f, "%.3f");
    else if (m_op == ImGuizmo::ROTATE) ImGui::DragFloat("##snapR", &m_snapRotate, 0.1f, 0.1f, 180.0f, "%.1f deg");
    else ImGui::DragFloat("##snapS", &m_snapScale, 0.01f, 0.001f, 10.0f, "%.3f");

    // Focus / history / settings.
    ImGui::SameLine(0, 12);
    ImGui::BeginDisabled(!target);
    if (ImGui::Button("Focus"))
        focus(target);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Frame the selection and orbit around it (F)");
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(!m_history.can_undo());
    if (ImGui::Button("Undo"))
        apply_history(m_history.undo(), true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo transform (Ctrl+Z)");
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(!m_history.can_redo());
    if (ImGui::Button("Redo"))
        apply_history(m_history.redo(), false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Redo transform (Ctrl+Y / Ctrl+Shift+Z)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("..."))
        ImGui::OpenPopup("##viewport_settings");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Viewport settings");
    if (ImGui::BeginPopup("##viewport_settings")) {
        ImGui::SeparatorText("Selection outline");
        if (m_renderer) {
            bool on = m_renderer->get_outline_enabled();
            if (ImGui::Checkbox("Show outline", &on)) m_renderer->set_outline_enabled(on);
            Vec4 c = m_renderer->get_outline_color();
            if (ImGui::ColorEdit3("Color", glm::value_ptr(c), ImGuiColorEditFlags_Float)) m_renderer->set_outline_color(c);
            float w = m_renderer->get_outline_width();
            if (ImGui::SliderFloat("Width (px)", &w, 0.5f, 8.0f, "%.1f")) m_renderer->set_outline_width(w);
            float h = m_renderer->get_outline_hidden_alpha();
            if (ImGui::SliderFloat("Occluded opacity", &h, 0.0f, 1.0f, "%.2f")) m_renderer->set_outline_hidden_alpha(h);
        }
        ImGui::SeparatorText("Picking");
        ImGui::SliderFloat("Strand hit radius (px)", &m_pickTolerancePx, 1.0f, 12.0f, "%.1f");
        ImGui::EndPopup();
    }

    m_toolbarWidth = ImGui::GetWindowWidth();

    // Second line: what is selected, plus any caveat for this kind of object.
    const char* kind = sel->get_type() == ObjectType::LIGHT    ? "light"
                       : sel->get_type() == ObjectType::CAMERA ? "camera"
                                                               : "mesh";
    ImGui::TextDisabled("%s", kind);
    ImGui::SameLine();
    ImGui::TextUnformatted(sel->get_name().c_str());
    if (target && target != sel) {
        ImGui::SameLine();
        ImGui::TextDisabled("(moves %s)", target->get_name().c_str());
    }
    if (!target) {
        ImGui::SameLine();
        ImGui::TextDisabled("- no gizmo; use the viewport camera controls");
    } else if (m_binders) {
        for (auto* b : *m_binders)
            if (b && b->hair_mesh() == target && b->is_bound()) {
                ImGui::SameLine();
                ImGui::TextColored(gui_theme::warning_color(), "bound to %s - re-Bind after moving",
                                   b->head_mesh() ? b->head_mesh()->get_name().c_str() : "head");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("This hair is attached to the scalp surface. Moving it offsets it from the skin;\n"
                                      "re-seat it with Bind in the HAIR BINDING panel.");
                break;
            }
    }
    ImGui::End();
}

void ViewportWidget::manipulate(Core::Object3D* target) {
    Core::Camera* cam = m_scene->get_active_camera();
    if (!cam || !op_allowed(target, m_op))
        return;
    ImGuiIO& io = ImGui::GetIO();

    // Draw over the 3D view but under the ImGui windows.
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

    Mat4 view = cam->get_view();
    // The engine projection carries a Vulkan Y-flip (m_proj[1][1] *= -1) that
    // ImGuizmo doesn't expect; undo it on the copy we hand the gizmo, or the
    // manipulator draws mirrored and drags invert vertically.
    Mat4 proj = cam->get_projection();
    proj[1][1] *= -1.0f;

    const bool dirLight = is_light(target, LightType::DIRECTIONAL);
    const Mat4 objWorld = target->get_model_matrix();

    // The matrix ImGuizmo manipulates. For meshes it shares the object's basis
    // (optionally re-seated at the geometry center); for a directional light it
    // is a frame whose +Z is the light direction, so rotating it aims the light.
    Mat4 gizmo = objWorld;
    if (dirLight) {
        if (!ImGuizmo::IsUsing() || !m_lightFrameLive)
            m_lightFrame = frame_from_direction(static_cast<Core::DirectionalLight*>(target)->get_direction());
        gizmo    = Mat4(m_lightFrame);
        gizmo[3] = objWorld[3];
    } else if (target->get_type() == ObjectType::LIGHT) {
        gizmo    = Mat4(1.0f); // point/spot: position only
        gizmo[3] = objWorld[3];
    } else if (m_pivotToGeometry) {
        Vec3 localCenter;
        if (mesh_geometry_center_local(target, localCenter))
            gizmo[3] = objWorld * Vec4(localCenter, 1.0f);
    }
    Mat4 gizmoAfter = gizmo;

    // Hold Ctrl to invert the Snap toggle for this drag.
    const bool snapping = m_useSnap != io.KeyCtrl;
    float      snap[3]  = {0.0f, 0.0f, 0.0f};
    if (snapping) {
        const float s = (m_op == ImGuizmo::TRANSLATE) ? m_snapTranslate : (m_op == ImGuizmo::ROTATE) ? m_snapRotate : m_snapScale;
        snap[0] = snap[1] = snap[2] = s;
    }

    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), m_op, m_mode,
                                              glm::value_ptr(gizmoAfter), nullptr, snapping ? snap : nullptr);
    m_lightFrameLive = ImGuizmo::IsUsing();
    if (!changed)
        return;

    Core::Object3D* parent   = target->get_parent();
    const Mat4      toParent = parent ? glm::inverse(parent->get_model_matrix()) : Mat4(1.0f);

    if (target->get_type() == ObjectType::LIGHT) {
        target->set_position(Vec3(toParent * gizmoAfter[3]));
        if (dirLight && m_op == ImGuizmo::ROTATE) {
            m_lightFrame = Mat3(gizmoAfter);
            static_cast<Core::DirectionalLight*>(target)->set_direction(glm::normalize(Vec3(gizmoAfter[2])));
        }
        return;
    }

    // The world-space delta the gizmo underwent (about its pivot), applied to
    // the object's real world matrix — so rotation/scale pivot about the gizmo,
    // not the object origin. With the pivot at the origin gizmo == objWorld and
    // this is a plain absolute manipulation.
    const Mat4 delta    = gizmoAfter * glm::inverse(gizmo);
    const Mat4 newLocal = toParent * (delta * objWorld);

    // Decompose in the ENGINE's Euler order (see transform_utils.h) — ImGuizmo's
    // own DecomposeMatrixToComponents assumes the opposite order and would
    // rewrite every multi-axis rotation.
    const transform_utils::TRS trs = transform_utils::decompose(newLocal, target->get_rotation());
    target->set_position(trs.position);
    target->set_rotation(trs.rotation);
    target->set_scale(trs.scale);
}

void ViewportWidget::draw_light_direction(Core::Object3D* target) {
    if (!is_light(target, LightType::DIRECTIONAL))
        return;
    Core::Camera* cam = m_scene->get_active_camera();
    if (!cam)
        return;
    // The stored direction points *toward* the light; draw the way light travels.
    const Vec3  pos  = Vec3(target->get_model_matrix()[3]);
    const Vec3  dir  = glm::normalize(static_cast<Core::DirectionalLight*>(target)->get_direction());
    const float len  = glm::distance(cam->get_position(), pos) * 0.25f;
    ImVec2      a, b;
    if (!project_to_screen(cam, pos, a) || !project_to_screen(cam, pos - dir * len, b))
        return;
    ImDrawList* dl  = ImGui::GetBackgroundDrawList();
    const ImU32 col = IM_COL32(255, 200, 60, 230);
    dl->AddLine(a, b, col, 2.0f);
    const ImVec2 d{b.x - a.x, b.y - a.y};
    const float  l = std::sqrt(d.x * d.x + d.y * d.y);
    if (l > 1.0f) {
        const ImVec2 u{d.x / l, d.y / l}, n{-u.y, u.x};
        dl->AddTriangleFilled(b, {b.x - u.x * 12 + n.x * 6, b.y - u.y * 12 + n.y * 6},
                              {b.x - u.x * 12 - n.x * 6, b.y - u.y * 12 - n.y * 6}, col);
    }
}

void ViewportWidget::watch_for_edits(Core::Object3D* target) {
    if (target != m_watched) {
        m_watched = target;
        m_touched = false;
        if (target)
            m_stable = TransformState::capture(target);
    }
    if (!target)
        return;

    const bool interacting = ImGui::IsAnyItemActive() || ImGuizmo::IsUsing();
    const TransformState now = TransformState::capture(target);
    if (interacting) {
        if (!now.same_as(m_stable))
            m_touched = true;
        return;
    }
    if (!now.same_as(m_stable)) {
        // Only user-driven changes become history; animation (light orbit, a
        // rebind that reparents) just moves the baseline.
        if (m_touched && now.parent == m_stable.parent)
            m_history.push({target, m_stable, now});
        m_stable = now;
    }
    m_touched = false;
}

void ViewportWidget::apply_history(const TransformHistory::Entry& e, bool undo) {
    if (!e.object)
        return;
    const TransformState& st = undo ? e.before : e.after;
    st.apply(e.object);
    // Show what changed; the watcher's baseline follows so this isn't re-recorded.
    m_selection->set_selected_object(e.object);
    m_watched = e.object;
    m_stable  = TransformState::capture(e.object);
    m_touched = false;
}

void ViewportWidget::focus(Core::Object3D* target) {
    Core::Camera* cam = m_scene->get_active_camera();
    if (!target || !cam)
        return;

    Vec3  center;
    float radius;
    Vec3  mn, mx;
    if (mesh_world_bounds(target, mn, mx)) {
        center = (mn + mx) * 0.5f;
        radius = std::max(glm::length(mx - mn) * 0.5f, 1e-3f);
    } else {
        center = Vec3(target->get_model_matrix()[3]);
        radius = 0.5f;
    }

    // Fit the bounding sphere in the narrower of the two fields of view.
    const ImVec2 size    = ImGui::GetIO().DisplaySize;
    const float  aspect  = size.y > 0.0f ? size.x / size.y : 1.0f;
    const float  halfV   = glm::radians(cam->get_field_of_view()) * 0.5f;
    const float  halfH   = std::atan(std::tan(halfV) * aspect);
    const float  halfFov = std::min(halfV, halfH);
    const float  dist    = std::max(radius / std::sin(halfFov) * 1.1f, cam->get_near() * 4.0f);

    // Keep the viewing direction; slide the camera so the selection is centred.
    const Vec3 forward = glm::normalize(cam->get_transform().forward);
    cam->set_position(center - forward * dist);
    if (m_controller)
        m_controller->set_orbital_center(center);
}

void ViewportWidget::handle_click() {
    ImGuiIO&   io        = ImGui::GetIO();
    const bool overGizmo = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_pressInViewport = !io.WantCaptureMouse && !overGizmo;

    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !m_pressInViewport)
        return;
    m_pressInViewport = false;

    // Left-drag orbits the camera: only a (near-)stationary click selects.
    if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] > 4.0f * 4.0f)
        return;

    const picking::Hit hit = picking::pick(m_scene,
                                           m_scene->get_active_camera(),
                                           Vec2(io.MousePos.x, io.MousePos.y),
                                           Vec2(io.DisplaySize.x, io.DisplaySize.y),
                                           m_binders ? *m_binders : std::vector<hair_binding::HairBinder*>{},
                                           m_pickTolerancePx);
    m_selection->set_selected_object(hit.object); // nullptr (background) deselects
}

void ThemeWidget::render() {
    ImGui::SeparatorText("Interface");
    const gui_theme::Theme cur = gui_theme::current();
    if (ImGui::BeginCombo("Theme", gui_theme::name(cur))) {
        for (int i = 0; i < (int)gui_theme::Theme::Count; ++i) {
            const auto t = (gui_theme::Theme)i;
            if (ImGui::Selectable(gui_theme::name(t), t == cur))
                gui_theme::set_current(t);
        }
        ImGui::EndCombo();
    }
}

void UserInterface::init(Core::IWindow*                          window,
                         Core::Scene*                            scene,
                         Systems::BaseRenderer*                  renderer,
                         Tools::Controller*                      controller,
                         std::vector<hair_binding::HairBinder*>* binders,
                         bool*                                   animateLight) {

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
    explorerPanel->add_child(new ThemeWidget());

    overlay->add_panel(explorerPanel);
    explorer = explorerPanel;

    Tools::Panel* propertiesPanel =
        new Tools::Panel("OBJECT PROPERTIES", 0.75f, 0, 0.25f, 0.8f, PanelWidgetFlags::NoMove, true);
    objectWidget = new Tools::ObjectExplorerWidget();
    propertiesPanel->add_child(objectWidget);

    overlay->add_panel(propertiesPanel);
    properties = propertiesPanel;

    gui_theme::register_panel(explorerPanel);
    gui_theme::register_panel(propertiesPanel);
    overlay->set_style_callback([] { gui_theme::apply(); });

    // Viewport tools (toolbar, gizmo, picking) — drawn every frame, independent
    // of which panels are open.
    viewport = new ViewportWidget(scene, sceneWidget, controller, binders, static_cast<Systems::ForwardRenderer*>(renderer));
    overlay->add_viewport_widget(viewport);
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
