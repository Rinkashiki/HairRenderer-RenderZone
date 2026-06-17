#include "gui.h"
#include <glm/gtc/type_ptr.hpp>

void HairBindWidget::render() {
    ImGui::TextUnformatted("HAIR → SCALP BINDING");
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
    objectWidget = new Tools::ObjectExplorerWidget();
    propertiesPanel->add_child(objectWidget);

    overlay->add_panel(propertiesPanel);
    properties = propertiesPanel;
}