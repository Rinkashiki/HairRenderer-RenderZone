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

# Pull in the shared scene loader and hair loader from the application layer.
list(APPEND SLVIEWER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/scene_loader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hair_loader.cpp"
)
list(APPEND SLVIEWER_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/scene_loader.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hair_loader.h"
)

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
# 5b. SLViewer — Embedded SPIR-V (pre-compiled shaders baked into the binary)
# -----------------------------------------------------------------------------
# The distributable must not ship GLSL source under resources/shaders/, so
# tools/precompile_shaders.py runs at build time to:
#   1. split each unified .glsl into per-stage GLSL (mirroring the engine's
#      ShaderSource::read_file logic, including #include from shaders/scripts/),
#   2. compile each stage with glslc -> SPIR-V,
#   3. run spirv-opt --strip-debug so reconstructed GLSL loses original names,
#   4. emit a generated embedded_shaders.cpp registering the blobs at static
#      init via shader_registry.h.
# The generated source is linked only into SLViewer; HairViewer continues to
# read .glsl files at runtime and compile via Shaderc.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# glslc + spirv-opt ship in the Vulkan SDK. find_package(Vulkan) above doesn't
# locate them on every platform — search next to the validation utility / SDK
# bin dir explicitly.
find_program(GLSLC_EXECUTABLE
    NAMES glslc
    HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
    DOC "glslc — Vulkan SDK GLSL->SPIR-V compiler")
find_program(SPIRV_OPT_EXECUTABLE
    NAMES spirv-opt
    HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
    DOC "spirv-opt — SPIR-V optimizer")

if(NOT GLSLC_EXECUTABLE OR NOT SPIRV_OPT_EXECUTABLE)
    message(FATAL_ERROR
        "SLViewer requires glslc and spirv-opt for shader embedding "
        "(install the Vulkan SDK and ensure VULKAN_SDK is set).")
endif()

set(SLVIEWER_EMBEDDED_SHADERS_CPP "${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp")
set(SLVIEWER_SHADER_ROOT          "${CMAKE_CURRENT_SOURCE_DIR}/ext/Vulkan-Engine/resources/shaders")
set(SLVIEWER_PRECOMPILE_SCRIPT    "${CMAKE_CURRENT_SOURCE_DIR}/tools/precompile_shaders.py")

# Re-run codegen whenever any .glsl under the engine shader tree changes.
file(GLOB_RECURSE SLVIEWER_SHADER_INPUTS "${SLVIEWER_SHADER_ROOT}/*.glsl")

add_custom_command(
    OUTPUT  "${SLVIEWER_EMBEDDED_SHADERS_CPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${SLVIEWER_PRECOMPILE_SCRIPT}"
            --shaders-root "${SLVIEWER_SHADER_ROOT}"
            --output       "${SLVIEWER_EMBEDDED_SHADERS_CPP}"
            --glslc        "${GLSLC_EXECUTABLE}"
            --spirv-opt    "${SPIRV_OPT_EXECUTABLE}"
    DEPENDS ${SLVIEWER_SHADER_INPUTS} "${SLVIEWER_PRECOMPILE_SCRIPT}"
    COMMENT "[SLViewer] Pre-compiling ${CMAKE_MATCH_COUNT}shaders to embedded SPIR-V"
    VERBATIM
)

target_sources(SLViewer PRIVATE "${SLVIEWER_EMBEDDED_SHADERS_CPP}")
set_source_files_properties("${SLVIEWER_EMBEDDED_SHADERS_CPP}"
    PROPERTIES GENERATED TRUE)

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

# Application-side runtime resources: models, textures, animations.
# Installed as full trees so any shipped scene (resources/scenes/*.json) resolves
# its references without per-scene CMake edits. The alembic source clips under
# models/hair_strands_ABCextraction/ are pre-process inputs (Blender + Python
# convert them to .hair) and are excluded from the install.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/models"
        DESTINATION resources
        PATTERN "hair_strands_ABCextraction" EXCLUDE)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/textures"
        DESTINATION resources)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/animations"
        DESTINATION resources)

# Scene definitions (default scene + any alternatives shipped for SLViewer's --scene flag)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/scenes"
        DESTINATION resources
        FILES_MATCHING PATTERN "*.json")

# Engine resources — meshes and textures (LUTs etc.). Shaders are NOT installed:
# SLViewer ships them as embedded SPIR-V (see "5b. Embedded SPIR-V" above), so
# resources/shaders/ on disk would be dead weight and would leak GLSL source.
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
