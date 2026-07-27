cmake_minimum_required(VERSION 3.16)
project(imgui)

add_library(imgui)
target_sources(imgui PRIVATE 
"${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_demo.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_draw.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_tables.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_widgets.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_glfw.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_vulkan.cpp"
# ImGuizmo (transform gizmos) is compiled straight into the imgui target so it
# shares imgui.h / imgui_internal.h and the single GImGui context, and links
# wherever imgui does.
"${CMAKE_CURRENT_SOURCE_DIR}/../imguizmo/ImGuizmo.cpp"
)

target_include_directories(imgui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../imguizmo")


target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)