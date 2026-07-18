#ifndef APP_H
#define APP_H

#include <chrono>

#include <engine/core.h>
#include <engine/systems.h>
#include <engine/tools/controller.h>

#include "gui.h"
#include "hair_loader.h"
#include "scene_loader.h"

USING_VULKAN_ENGINE_NAMESPACE

using namespace Core;

class HairViewer
{
    // Single source of truth for the loaded scene. Referenced by both
    // init() (to peek renderer.msaa before constructing the renderer) and
    // setup() (to actually load the scene).
    static constexpr const char* SCENE_PATH = RESOURCES_PATH "scenes/eyes.json";

    UserInterface m_interface{};

    Core::IWindow*         m_window;
    Systems::BaseRenderer* m_renderer;
    Scene*                 m_scene;
    Camera*                camera;
    Tools::Controller*     m_controller;

    // Surface binders for strand-hair meshes (scalp hair, brows, lashes).
    std::vector<hair_binding::HairBinder*> m_binders;

    bool animateLight{false};
    bool freezeAnimation{false}; // Press P to freeze/unfreeze the animation pose

    int m_maxFrames{0};
    int m_frameCount{0};

    struct Time {
        float delta{0.0f};
        float last{0.0f};
        float framesPerSecond{0.0f};
    };
    Time m_time{};

  public:
    void init(Systems::RendererSettings settings);

    void run(Systems::RendererSettings settings);

    void set_max_frames(int n) { m_maxFrames = n; }

  private:
    void setup();

    // Create hair surface binders. If the scene declared bind_to requests, use
    // them (and their explicit head + sidecar); otherwise auto-discover the head
    // and strand-hair meshes. Auto-loads a sidecar (<hair file>.hbnd or the
    // declared path) when present.
    void setup_hair_binding(const std::vector<scene_loader::HairBindRequest>& requests);

    void tick();

    void update();

#pragma region Input Management

    void keyboard_callback(int key, int scancode, int action, int mods) {
        void* windowHandle{nullptr};
        m_window->get_handle(windowHandle);
        GLFWwindow* glfwWindow = static_cast<GLFWwindow*>(windowHandle);
        if (glfwGetKey(glfwWindow, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            m_window->set_window_should_close(true);
        }

        if (glfwGetKey(glfwWindow, GLFW_KEY_F11) == GLFW_PRESS)
        {
            m_window->set_fullscreen(m_window->is_fullscreen() ? false : true);
        }
        if (glfwGetKey(glfwWindow, GLFW_KEY_L) == GLFW_PRESS)
        {
            animateLight = animateLight ? false : true;
        }
        if (glfwGetKey(glfwWindow, GLFW_KEY_P) == GLFW_PRESS)
        {
            freezeAnimation = freezeAnimation ? false : true;
        }
    }

    void mouse_callback(double xpos, double ypos) {
        if (m_interface.overlay->wants_to_handle_input())
            return;

        m_controller->handle_mouse((float)xpos, (float)ypos);
    }

    void window_resize_callback(int width, int height) {
        m_window->set_size(width, height);
        m_interface.overlay->set_extent({width, height});
    }

#pragma endregion
};

#endif