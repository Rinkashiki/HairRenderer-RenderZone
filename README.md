# VULKAN STRAND-BASED HAIR RENDERER (WIP)

* Available only on Windows (for now).
* Unity's Enemies Hair Pipeline
* Some hair models are created from [Gaussian Haircut](https://github.com/eth-ait/GaussianHaircut).

## PREQUISITES

- Windows 10, 11 (Although it should be easy enough to set it up for Linux).
- Vulkan SDK 1.3.* installed (With VMA and Shaderc).
- CMake installed.
- Git LFS (resources are stored with it)
- Ninja (recommended)

1. Clone the repo:
   ```bash
   git clone --recursive https://github.com/AEspinosaDev/Hair-Renderer-2.git
   cd Hair-Renderer-X
   ```
2. Build with CMake:
   ```bash
   mkdir build
   cd build
   cmake ..
   ```

## CONTROLS (HairViewer)

### Camera & app
| Input | Action |
|-------|--------|
| `W` / `A` / `S` / `D` | Move camera forward / left / back / right |
| `E` / `Q` | Move camera up / down |
| `R` | Reset camera to its initial pose |
| Left-mouse drag | Orbit / look (camera is in `ORBITAL` mode) |
| `F11` | Toggle fullscreen |
| `L` | Toggle light animation |
| `P` | Freeze / unfreeze the animation pose |
| `Esc` | Quit |

### Transform gizmo
Pick an object in the **EXPLORER** panel to target it; the gizmo is driven from
the **OBJECT PROPERTIES** panel (TRANSFORM GIZMO section) and drawn over the 3D
view. Drag an axis/plane/ring to move, rotate or scale the selection. Camera
look is suppressed while the pointer is over or dragging the gizmo, so the two
never fight.

| Input | Action |
|-------|--------|
| `1` / `2` / `3` | Switch to Move / Rotate / Scale |
| `X` | Toggle World ↔ Local space |
| Panel: **Pivot at geometry center** | Draw & pivot the gizmo at the mesh's geometry-bounds center instead of its object origin (non-destructive; meshes only) |
| Panel: **Snap** + step | Constrain drags to a grid / angle / scale step |

> Gizmo hotkeys are `1/2/3` and `X` (not the usual `W/E/R`) on purpose — the
> camera already owns `W`, `E` and `R`.

## PROJECT STRUCTURE

Applicaiton links against an older version of Vulkan-Engine (pretty rusty version but enough for demos).

Application loads an avatar and a hair mesh and renders it.

### Application Main Components

* **`HairViewer`**: The core of the application. It orchestrates the entire program lifecycle, from initialization (`init`, `setup`) to the main execution loop (`run`, `tick`, `update`).
* **`WindowGLFW` & `ForwardRenderer`**: `WindowGLFW` handles the application window and inputs (keyboard/mouse), while `Systems::ForwardRenderer` is the graphics engine responsible for drawing the scene and computing shadows.
* **`Scene`, `Camera`, & `Controller`**: 
    * `Scene`: Acts as the master container holding all 3D objects and lights.
    * `Camera`: Navigated using a `Tools::Controller` set to `ORBITAL` mode, allowing for easy inspection of the 3D models.
* **Lighting System**:
    * **`PointLight`**: The primary light source casting dynamic shadows (with configurable bias, FOV, near planes, etc.).
    * **`Skybox` / `TextureHDR`**: Handles environment map (HDRI) loading for ambient lighting and reflections.
* **Meshes & Materials (`Mesh`)**:
    * **`PhysicallyBasedMaterial`**: Standard PBR materials used for the character's head and eyes (supporting albedo, roughness, and metalness).
    * **`HairEpicMaterial`**: A specialized material with advanced parameters (such as strand thickness) designed specifically for realistic hair rendering.
* **Resource Loading & Neural Avatars**: Utilizes `Tools::Loaders` for standard assets (PLY files, textures). For heavy neural hair models, it implements a multi-threaded approach (`std::thread` via `hair_loaders::load_neural_hair`) to load assets in the background without freezing the application.
* **User Interface (`m_interface`)**: Manages the overlay GUI. It provides a scene hierarchy viewer (`sceneWidget`), real-time editing of object properties (`objectWidget`), and an **ImGuizmo-based transform gizmo** (`gizmoWidget`) for moving/rotating/scaling the selected object directly in the viewport (see **CONTROLS** above).

### ⚙️ Vulkan Engine Details

#### Shader System

* **No Reflection**: Automatic shader reflection is not implemented. Descriptor layouts must be manually defined in the C++ Pass implementation.
* **Include System**: A simple, non-recursive include system is available. Shaders are organized by type and utility. All module includes must be declared in the entry-point shader:
```glsl
#version 450
#include utils.glsl
#include BRDFs/epic_hair_BSDF.glsl
#include montecarlo.glsl
```
* **Unified Shader Files**: You can write the entire graphics pipeline (vertex, fragment, etc.) inside a single file using compilation directives. The engine compiles shaders on the fly automatically.
```glsl
#version 450
#shader fragment
```
Shaders are build on the fly autamitaclly, so no need of using no .bat or thir party executable.

### Renderer & RenderPasses

Currently, both **Forward** and **Deferred** renderers are implemented. For hair rendering, the **Forward Renderer is highly advised**. Hair fibers are high-frequency primitives that benefit greatly from hardware MSAA, as standard TAA is usually insufficient to preserve fine strand details.

* **No Render Graph**: Render passes are directly created and sequenced at renderer startup.
* **Dependency Tables**: Resources (Render Targets, UAVs) are managed via a dependency table. To add new passes with new resources, you must increase the Render Target (RT) pool and update the dependency table.
* **Atomic Operations**: One RenderPass should represent an atomic render operation (e.g., a single compute pass, a G-Buffer pass, or a scattering pass). While you can pack multiple operations into one pass, resource synchronization becomes difficult.
* **RHI Isolation**: The RHI (Render Hardware Interface) module resides in the Graphics folder and should generally remain untouched.


#### Working with IBasePass
Render passes live in the Core/Passes folder and follow a classic OOP inheritance model based on IBasePass.
Note: This engine uses classic Vulkan RenderPass objects, not the modern VK_KHR_dynamic_rendering extension.

```c
#pragma region Core Functions

// Setup attachments (Render Targets) and dependencies for this pass
virtual void setup_attachments(std::vector<Graphics::AttachmentInfo>& attachments,
                               std::vector<Graphics::SubPassDependency>& dependencies) = 0;

// Declare descriptor layouts and sets here
virtual void setup_uniforms(std::vector<Graphics::Frame>& frames) = 0;

// Define Graphics/Compute pipelines and link shaders
virtual void setup_shader_passes() = 0;

// Master setup function: calls setup_attachments, setup_uniforms, etc.
void setup(std::vector<Graphics::Frame>& frames);

// The actual execution of the rendering pipeline/commands
virtual void render(Graphics::Frame& currentFrame, Scene* const scene, uint32_t presentImageIndex = 0) = 0;

virtual void update_uniforms(uint32_t frameIndex, Scene* const scene) {}
virtual void link_previous_images(std::vector<Graphics::Image> images) {}

// Framebuffer management
virtual void create_framebuffer();
virtual void clean_framebuffer();

// Handles recreation of the pass (e.g., on window resize)
virtual void update();

// Destroys the renderpass and its shader passes
virtual void cleanup();

#pragma endregion
```

#### How to Add a New Render Pass
1. **Inherit**: Create a new class inheriting from IBasePass.

2. **Implement Setups**: * setup_attachments(): Declare the render targets.

3. **setup_uniforms()**: Define descriptor layouts and pools.

4. **setup_shader_passes()**: Define your pipelines and link your shaders.

5. **Implement Logic**: Write your command buffer recording logic inside the render() function.

6. **Instantiate**: Add your new pass to the Renderer's setup_passes() function.

7. **Link**: Link the necessary resources (inputs/outputs) to it using the dependency table.

## 📂 Files of Interest

### Shaders:

* ext/Vulkan-Engine/resources/shaders/scripts/BRDFs/epic_hair_BSDF.glsl (Hair shader implementation)

* ext/Vulkan-Engine/resources/shaders/scripts/BRDFs/schlick_smith_BRDF.glsl (Standard PBR shader)

### Renderers & Passes:

* ext/Vulkan-Engine/src/systems/renderers/forward.cpp (Forward Renderer logic)

* ext/Vulkan-Engine/src/core/passes/forward_pass.cpp (Forward Pass implementation)

* ext/Vulkan-Engine/src/core/passes/hair_scattering_pass.cpp (Example of a pure Compute Pass)

### Resource Management:

* ext/Vulkan-Engine/src/core/resource_manager.cpp (Crucial class where CPU data is arranged and uploaded to the GPU)

### UI:

* ext\Vulkan-Engine\include\engine\tools\widgets.h



