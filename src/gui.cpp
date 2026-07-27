#include "gui.h"
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
    const std::string side = hair->get_file_route() + ".hbnd";
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