cmake_minimum_required(VERSION 3.16)

project(HairRenderer VERSION 1.0.0)

# -----------------------------------------------------------------------------
# 1. Standard C++ Setup
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime — avoids VC++ redistributable dependency in the install tree.
# /MT in Release, /MTd in Debug. Both executables benefit.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
 
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

# Installed binary looks for .so files in lib/ next to itself — no LD_LIBRARY_PATH wrapper needed.
if(UNIX AND NOT APPLE)
    set_target_properties(SLViewer PROPERTIES
        INSTALL_RPATH "$ORIGIN/lib"
        BUILD_WITH_INSTALL_RPATH FALSE
    )
endif()

# -----------------------------------------------------------------------------
# 6. SLViewer — Bundle static ffmpeg
# -----------------------------------------------------------------------------
set(_ffmpeg_bin "")
set(_ffmpeg_dl_dir "${CMAKE_BINARY_DIR}/ffmpeg-download")

if(UNIX AND NOT APPLE)
    set(_ffmpeg_bin "${_ffmpeg_dl_dir}/ffmpeg")

    if(NOT EXISTS "${_ffmpeg_bin}")
        message(STATUS "[SLViewer] Downloading static ffmpeg GPL build (linux64) from BtbN...")
        file(MAKE_DIRECTORY "${_ffmpeg_dl_dir}")
        file(DOWNLOAD
            "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-gpl.tar.xz"
            "${_ffmpeg_dl_dir}/ffmpeg.tar.xz"
            SHOW_PROGRESS
            TLS_VERIFY ON
            STATUS _dl_status
        )
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            list(GET _dl_status 1 _dl_msg)
            message(WARNING "[SLViewer] ffmpeg download failed (${_dl_msg}). System ffmpeg will be used at runtime.")
            set(_ffmpeg_bin "")
        else()
            execute_process(
                COMMAND tar -xf "${_ffmpeg_dl_dir}/ffmpeg.tar.xz"
                        --strip-components=2
                        -C "${_ffmpeg_dl_dir}"
                        --wildcards
                        "*/bin/ffmpeg"
                RESULT_VARIABLE _tar_result
            )
            if(NOT _tar_result EQUAL 0 OR NOT EXISTS "${_ffmpeg_bin}")
                message(WARNING "[SLViewer] ffmpeg extraction failed. System ffmpeg will be used at runtime.")
                set(_ffmpeg_bin "")
            else()
                execute_process(COMMAND chmod +x "${_ffmpeg_bin}")
                message(STATUS "[SLViewer] ffmpeg binary ready: ${_ffmpeg_bin}")
            endif()
        endif()
    else()
        message(STATUS "[SLViewer] Using cached ffmpeg: ${_ffmpeg_bin}")
    endif()

elseif(WIN32)
    set(_ffmpeg_bin "${_ffmpeg_dl_dir}/ffmpeg.exe")

    if(NOT EXISTS "${_ffmpeg_bin}")
        message(STATUS "[SLViewer] Downloading static ffmpeg GPL build (win64) from BtbN...")
        file(MAKE_DIRECTORY "${_ffmpeg_dl_dir}")
        file(DOWNLOAD
            "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
            "${_ffmpeg_dl_dir}/ffmpeg.zip"
            SHOW_PROGRESS
            TLS_VERIFY ON
            STATUS _dl_status
        )
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            list(GET _dl_status 1 _dl_msg)
            message(WARNING "[SLViewer] ffmpeg download failed (${_dl_msg}). System ffmpeg will be used at runtime.")
            set(_ffmpeg_bin "")
        else()
            # CMake's built-in tar handles .zip on all platforms
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E tar xf "${_ffmpeg_dl_dir}/ffmpeg.zip"
                WORKING_DIRECTORY "${_ffmpeg_dl_dir}"
                RESULT_VARIABLE _unzip_result
            )
            # Locate the extracted ffmpeg.exe anywhere under the download dir
            file(GLOB_RECURSE _ffmpeg_candidates "${_ffmpeg_dl_dir}/*/bin/ffmpeg.exe")
            if(_ffmpeg_candidates)
                list(GET _ffmpeg_candidates 0 _ffmpeg_src)
                file(COPY "${_ffmpeg_src}" DESTINATION "${_ffmpeg_dl_dir}")
                message(STATUS "[SLViewer] ffmpeg.exe ready: ${_ffmpeg_bin}")
            else()
                message(WARNING "[SLViewer] ffmpeg.exe not found after extraction. System ffmpeg will be used at runtime.")
                set(_ffmpeg_bin "")
            endif()
        endif()
    else()
        message(STATUS "[SLViewer] Using cached ffmpeg.exe: ${_ffmpeg_bin}")
    endif()
endif()

# -----------------------------------------------------------------------------
# 7. Install rules
# -----------------------------------------------------------------------------
install(TARGETS SLViewer RUNTIME DESTINATION .)
if(NOT "${_ffmpeg_bin}" STREQUAL "" AND EXISTS "${_ffmpeg_bin}")
    install(PROGRAMS "${_ffmpeg_bin}" DESTINATION .)
endif()

# Legal
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt" DESTINATION .)

# Alex scene — models
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/models/alex/alex.glb"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/models/alex/hair_fauxmohawk.obj"
    DESTINATION resources/models/alex
)

# Alex scene — textures
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/textures/alex/hair_fauxmohawk_attribute.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/textures/alex/hair_fauxmohawk_tangent.png"
    DESTINATION resources/textures/alex
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/textures/studio_demo.hdr"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/textures/scatterDistance.png"
    DESTINATION resources/textures
)

# Sample animation files (used as --input to SLViewer)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/animations/test_anim.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/animations/test_morph.json"
    DESTINATION resources/animations
)

# Engine resources — shaders (compiled on-the-fly), meshes, and textures (LUTs etc.)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ext/Vulkan-Engine/resources/shaders"
        DESTINATION resources)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ext/Vulkan-Engine/resources/meshes"
        DESTINATION resources)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ext/Vulkan-Engine/resources/textures"
        DESTINATION resources)

# -----------------------------------------------------------------------------
# 8. Windows runtime DLLs (vulkan-1.dll)
# -----------------------------------------------------------------------------
if(WIN32)
    # Prefer VULKAN_SDK env var (set by LunarG installer).
    # Fall back to the directory next to the import library found by find_package.
    set(_vulkan_dll "")
    if(DEFINED ENV{VULKAN_SDK})
        set(_vulkan_dll "$ENV{VULKAN_SDK}/Bin/vulkan-1.dll")
    elseif(DEFINED Vulkan_LIBRARY)
        get_filename_component(_vulkan_lib_dir "${Vulkan_LIBRARY}" DIRECTORY)
        set(_vulkan_dll "${_vulkan_lib_dir}/vulkan-1.dll")
    endif()

    if(NOT "${_vulkan_dll}" STREQUAL "" AND EXISTS "${_vulkan_dll}")
        install(FILES "${_vulkan_dll}" DESTINATION .)
        message(STATUS "[SLViewer] vulkan-1.dll found: ${_vulkan_dll}")
    else()
        message(WARNING "[SLViewer] vulkan-1.dll not found — deployment on a machine without the Vulkan runtime will fail. Set VULKAN_SDK or install the LunarG Vulkan SDK.")
    endif()
endif()

# -----------------------------------------------------------------------------
# 9. Linux runtime library (libvulkan.so.1)
# -----------------------------------------------------------------------------
# We do NOT bundle libvulkan.so.1. On Linux the Vulkan loader ships with GPU
# drivers (mesa-vulkan-drivers, nvidia-driver, etc.) and knows how to locate
# ICDs via /usr/share/vulkan/icd.d/. Bundling an SDK copy of the loader breaks
# on other machines because that copy looks for ICDs in SDK-specific paths that
# don't exist, causing vkCreateDevice to fail even when the GPU driver is fine.
# Any machine capable of running Vulkan already has the system loader installed.
