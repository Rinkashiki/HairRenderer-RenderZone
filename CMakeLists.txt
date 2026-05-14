cmake_minimum_required(VERSION 3.16)

project(HairRenderer VERSION 1.0.0)
 
# -----------------------------------------------------------------------------
# 1. Standard C++ Setup
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
 
# compile_commands.json is useful for Clangd/LSP on Linux
if(NOT MSVC)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()
 
# -----------------------------------------------------------------------------
# 2. Dependency Discovery
# -----------------------------------------------------------------------------
 
# Find Vulkan - Standard CMake module
find_package(Vulkan REQUIRED)
if(NOT Vulkan_FOUND)
    message(FATAL_ERROR "Vulkan SDK not found. 
    Windows: Install LunarG SDK and set VULKAN_SDK env var.
    Linux: Source 'setup-env.sh' from your SDK folder.")
endif()
 
# Find Threads - Required for Linux std::thread support
find_package(Threads REQUIRED)
 
# -----------------------------------------------------------------------------
# 3. Subdirectories & External Engine
# -----------------------------------------------------------------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
 
# The Vulkan-Engine subdirectory likely handles its own Vulkan linking, 
# but we link explicitly in the app target for safety.
add_subdirectory(ext/Vulkan-Engine)
 
# -----------------------------------------------------------------------------
# 4. HairViewer Target
# -----------------------------------------------------------------------------
file(GLOB APP_SOURCES "src/*.cpp")
file(GLOB APP_HEADERS "src/*.h")

add_executable(HairViewer ${APP_SOURCES} ${APP_HEADERS})

target_link_libraries(HairViewer PRIVATE
    VulkanEngine
    Vulkan::Vulkan
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(HairViewer PRIVATE dl)
endif()

target_compile_definitions(HairViewer PUBLIC
    RESOURCES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/"
)

if(MSVC)
    target_compile_options(HairViewer PRIVATE /W4 /MP)
else()
    target_compile_options(HairViewer PRIVATE -Wall -Wextra -O3)
endif()

# -----------------------------------------------------------------------------
# 5. SLViewer Target (standalone deployment binary)
# -----------------------------------------------------------------------------
file(GLOB_RECURSE SLVIEWER_SOURCES "src/slviewer/*.cpp")
file(GLOB_RECURSE SLVIEWER_HEADERS "src/slviewer/*.h")

add_executable(SLViewer ${SLVIEWER_SOURCES} ${SLVIEWER_HEADERS})

target_link_libraries(SLViewer PRIVATE
    VulkanEngine
    Vulkan::Vulkan
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(SLViewer PRIVATE dl)
endif()

# RESOURCES_PATH is resolved at runtime from the exe directory (resource_paths.cpp).
# Define an empty string so any accidental macro use fails loudly at compile time.
target_compile_definitions(SLViewer PUBLIC
    RESOURCES_PATH=""
    SLVIEWER_BUILD=1
)

if(MSVC)
    target_compile_options(SLViewer PRIVATE /W4 /MP)
else()
    target_compile_options(SLViewer PRIVATE -Wall -Wextra -O3)
endif()
