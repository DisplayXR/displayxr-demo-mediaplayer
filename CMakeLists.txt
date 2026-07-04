# SPDX-License-Identifier: Apache-2.0
# DisplayXR Stereo Media Player — M0 skeleton.
# One Vulkan codebase for Windows / Linux / macOS (macOS via MoltenVK).
cmake_minimum_required(VERSION 3.24)  # FetchContent FIND_PACKAGE_ARGS
project(displayxr_mediaplayer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Platform-suffixed binary name: mediaplayer_handle_vk_{macos,win,linux}.
if(APPLE)
    set(MP_PLATFORM "macos")
elseif(WIN32)
    set(MP_PLATFORM "win")
else()
    set(MP_PLATFORM "linux")
endif()
set(MP_TARGET "mediaplayer_handle_vk_${MP_PLATFORM}")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# --- Vulkan (MoltenVK on macOS). Homebrew/SDK both resolve via FindVulkan.
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan: ${Vulkan_LIBRARY} (include ${Vulkan_INCLUDE_DIR})")

# --- OpenXR loader. Prefer an installed/dev loader (e.g. /tmp/openxr-install),
#     else build the pinned Khronos OpenXR-SDK loader from source.
set(OpenXR_FOUND FALSE)
find_package(OpenXR CONFIG QUIET)
if(OpenXR_FOUND)
    message(STATUS "OpenXR loader: found via find_package (OpenXR::openxr_loader)")
    set(MP_OPENXR_LOADER OpenXR::openxr_loader)
else()
    message(STATUS "OpenXR loader: not installed — fetching Khronos OpenXR-SDK")
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    # Link the loader against the system jsoncpp; its vendored copy is too old to
    # compile against current SDKs.
    set(BUILD_WITH_SYSTEM_JSONCPP ON CACHE BOOL "" FORCE)
    # jsoncpp config-mapping shim. The loader's bundled FindJsonCpp.cmake stamps a
    # MAP_IMPORTED_CONFIG_RELEASE onto jsoncpp's imported target, but Homebrew jsoncpp
    # ≥1.9.6 ships only a config-LESS IMPORTED_LOCATION — so a Release build maps to no
    # per-config location and links `jsoncpp_lib-NOTFOUND`. Seed the map (the module
    # APPENDs to it, so the leading "" entry survives) so Release accepts the config-
    # less location. Inherited by the loader subproject; read at generate time.
    set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE ";RELEASE;NOCONFIG")
    FetchContent_Declare(OpenXR
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.43
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(OpenXR)
    set(MP_OPENXR_LOADER openxr_loader)
endif()

# --- FFmpeg (libav*) for video decode (M2/M3). Optional: if absent, the app builds
#     without video support.
#   * macOS / Linux: pkg-config (Homebrew / system).
#   * Windows: no pkg-config in the usual MSVC toolchain, so fall back to a manual
#     dev tree. Set -DFFMPEG_ROOT=<dir containing include/ + lib/>. For the shipped
#     installer, CI builds a slim DECODE-ONLY FFmpeg via scripts/build-ffmpeg-slim.sh
#     (~9 MB of DLLs vs ~103 MB for a stock full build). For local dev that script
#     works too, or use any FFmpeg dev tree (a vcpkg 'installed/x64-windows' prefix,
#     or a BtbN/gyan.dev prebuilt).
find_package(PkgConfig QUIET)
set(MP_HAVE_FFMPEG FALSE)
set(MP_FFMPEG_LINK "")           # what the target links against (pkg-config target or libs)
set(MP_FFMPEG_DLL_DIR "")        # Windows: dir holding the runtime DLLs to stage by the exe
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    if(FFMPEG_FOUND)
        set(MP_HAVE_FFMPEG TRUE)
        set(MP_FFMPEG_LINK PkgConfig::FFMPEG)
        message(STATUS "FFmpeg: found via pkg-config (libavcodec ${FFMPEG_libavcodec_VERSION})")
    endif()
endif()
if(NOT MP_HAVE_FFMPEG)
    # Manual fallback. Honor FFMPEG_ROOT (cache or env); otherwise still let
    # find_*/CMAKE_PREFIX_PATH (e.g. a vcpkg toolchain) resolve the libs.
    if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
        set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
    endif()
    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${FFMPEG_ROOT}/include")
    find_library(FFMPEG_avcodec  avcodec  HINTS "${FFMPEG_ROOT}/lib")
    find_library(FFMPEG_avformat avformat HINTS "${FFMPEG_ROOT}/lib")
    find_library(FFMPEG_avutil   avutil   HINTS "${FFMPEG_ROOT}/lib")
    find_library(FFMPEG_swscale  swscale  HINTS "${FFMPEG_ROOT}/lib")
    find_library(FFMPEG_swresample swresample HINTS "${FFMPEG_ROOT}/lib")
    if(FFMPEG_INCLUDE_DIR AND FFMPEG_avcodec AND FFMPEG_avformat AND FFMPEG_avutil AND FFMPEG_swscale AND FFMPEG_swresample)
        add_library(mp_ffmpeg INTERFACE)
        target_include_directories(mp_ffmpeg INTERFACE "${FFMPEG_INCLUDE_DIR}")
        target_link_libraries(mp_ffmpeg INTERFACE
            "${FFMPEG_avcodec}" "${FFMPEG_avformat}" "${FFMPEG_avutil}" "${FFMPEG_swscale}"
            "${FFMPEG_swresample}")
        set(MP_HAVE_FFMPEG TRUE)
        set(MP_FFMPEG_LINK mp_ffmpeg)
        message(STATUS "FFmpeg: found via FFMPEG_ROOT='${FFMPEG_ROOT}' (${FFMPEG_INCLUDE_DIR})")
        # For shared builds (the common Windows prebuilts), stage the DLLs next to
        # the exe so it runs without PATH munging. Look beside the import libs.
        if(WIN32 AND FFMPEG_ROOT AND EXISTS "${FFMPEG_ROOT}/bin")
            set(MP_FFMPEG_DLL_DIR "${FFMPEG_ROOT}/bin")
        endif()
    endif()
endif()
if(NOT MP_HAVE_FFMPEG)
    message(STATUS "FFmpeg: not found — video playback (M2/M3) will be unavailable "
                   "(Windows: pass -DFFMPEG_ROOT=<ffmpeg dev tree>)")
endif()

# --- SDL3 (window + input + native handle). Pinned; uses an installed SDL3 if one
#     is already present, else builds the pinned source.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.10
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS NAMES SDL3)
FetchContent_MakeAvailable(SDL3)

# --- Dear ImGui (subtle 3D UI, M4). No upstream CMake target — compile the core
#     plus the SDL3 + Vulkan backends straight into our target.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

# --- nlohmann/json (header-only) for parsing the LIF container's JSON metadata.
#     Lightweight (no link step), so LIF support is on the default path — unlike the
#     heavier libheif behind MEDIAPLAYER_WITH_HEIF.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS NAMES nlohmann_json)
FetchContent_MakeAvailable(nlohmann_json)
set(MP_IMGUI_DIR "${imgui_SOURCE_DIR}")
set(MP_IMGUI_SOURCES
    ${MP_IMGUI_DIR}/imgui.cpp
    ${MP_IMGUI_DIR}/imgui_draw.cpp
    ${MP_IMGUI_DIR}/imgui_tables.cpp
    ${MP_IMGUI_DIR}/imgui_widgets.cpp
    ${MP_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${MP_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
add_executable(${MP_TARGET}
    src/main.cpp
    src/App.cpp
    src/Log.cpp
    src/platform/Window.cpp
    src/xr/XrSession.cpp
    src/rhi/VulkanRenderer.cpp
    src/media/ImageDecoder.cpp
    src/media/LifLoader.cpp
    src/media/MediaSource.cpp
    src/media/VideoDecoder.cpp
    src/media/AudioPlayer.cpp
    src/ui/Hud.cpp
    src/ui/ImGuiLayer.cpp
    src/media/FrameRing.h  # header-only triple buffer
    # Header-only stub (fleshed out in a later milestone):
    src/ui/Ui.h
    ${MP_IMGUI_SOURCES}
)

# Windows: link as a GUI (WINDOWS subsystem) app so launching the player never
# pops a console window — neither standalone (Explorer) nor under the shell. SDL3's
# <SDL3/SDL_main.h> supplies the WinMain entry that forwards to our main(), so the
# console subsystem is unnecessary. stderr still works when a console is attached
# (e.g. the shell's CREATE_NEW_CONSOLE) or redirected.
if(WIN32)
    set_target_properties(${MP_TARGET} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(${MP_TARGET} PRIVATE
    ${MP_IMGUI_DIR} ${MP_IMGUI_DIR}/backends)
target_compile_definitions(${MP_TARGET} PRIVATE MEDIAPLAYER_WITH_IMGUI=1)

if(MP_HAVE_FFMPEG)
    target_compile_definitions(${MP_TARGET} PRIVATE MEDIAPLAYER_WITH_FFMPEG=1)
    target_link_libraries(${MP_TARGET} PRIVATE ${MP_FFMPEG_LINK})
    # Windows shared FFmpeg: copy the runtime DLLs next to the binary post-build.
    if(MP_FFMPEG_DLL_DIR)
        file(GLOB _ffmpeg_dlls "${MP_FFMPEG_DLL_DIR}/*.dll")
        foreach(_dll ${_ffmpeg_dlls})
            add_custom_command(TARGET ${MP_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" "$<TARGET_FILE_DIR:${MP_TARGET}>"
                COMMENT "Staging FFmpeg DLL $<PATH:GET_FILENAME,${_dll}> next to ${MP_TARGET}")
        endforeach()
    endif()
endif()

target_include_directories(${MP_TARGET} BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/displayxr-openxr  # vendored <openxr/...>
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb               # stb_image_write.h
)

target_link_libraries(${MP_TARGET} PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    nlohmann_json::nlohmann_json
    ${MP_OPENXR_LOADER}
)

# Windows: SHGetKnownFolderPath (shell32) + CoTaskMemFree (ole32) for the 'I'-key
# atlas-capture output path under the user's Pictures folder.
# d3d11 + dxgi: zero-copy D3D11VA->Vulkan decode interop (issue #28).
if(WIN32)
    target_link_libraries(${MP_TARGET} PRIVATE shell32 ole32 d3d11 dxgi)
endif()

# Copy the OpenXR loader dylib/dll next to the binary when it's an imported target,
# so the app runs without extra PATH/DYLD setup.
if(TARGET OpenXR::openxr_loader)
    get_target_property(_oxr_loc OpenXR::openxr_loader IMPORTED_LOCATION)
    if(NOT _oxr_loc)
        get_target_property(_oxr_loc OpenXR::openxr_loader IMPORTED_LOCATION_RELEASE)
    endif()
    if(_oxr_loc)
        add_custom_command(TARGET ${MP_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_oxr_loc}" "$<TARGET_FILE_DIR:${MP_TARGET}>"
            COMMENT "Copying OpenXR loader next to ${MP_TARGET}")
    endif()
endif()

# Stage dependent shared-library DLLs (SDL3 when built shared, an OpenXR loader DLL
# if the loader is dynamic, etc.) next to the binary so it runs in-place on Windows
# without PATH munging. FFmpeg's DLLs come via MP_FFMPEG_DLL_DIR above (raw .lib
# paths, not CMake targets, so they're not in TARGET_RUNTIME_DLLS).
if(WIN32)
    add_custom_command(TARGET ${MP_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${MP_TARGET}> "$<TARGET_FILE_DIR:${MP_TARGET}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Staging dependent runtime DLLs next to ${MP_TARGET}")
endif()

# Sidecar workspace manifest + real logos (generated by /make-app-logos from a
# live atlas capture). Staged next to the binary on every platform so dev builds
# and the installer's BIN_DIR both carry them, and the idle logo resolves at
# <exe-dir>/displayxr/ (manifest spec) instead of falling back to the test pattern.
add_custom_command(TARGET ${MP_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/displayxr" "$<TARGET_FILE_DIR:${MP_TARGET}>/displayxr"
    COMMENT "Staging displayxr/ sidecar (manifest + logos) next to ${MP_TARGET}")

message(STATUS "Configured target: ${MP_TARGET}")

# ---------------------------------------------------------------------------
# Tests (opt-in: -DMP_BUILD_TESTS=ON). LifLoader container round-trip — builds
# synthetic stereo/mono/corrupt LIFs in memory and asserts the parse + SBS compose.
# Header-only deps (stb, nlohmann/json) + Log/ImageDecoder; no Vulkan/SDL/OpenXR.
# ---------------------------------------------------------------------------
option(MP_BUILD_TESTS "Build unit tests" OFF)
if(MP_BUILD_TESTS)
    enable_testing()
    add_executable(lif_roundtrip_test
        tests/lif_roundtrip_test.cpp
        src/media/LifLoader.cpp
        src/media/ImageDecoder.cpp
        src/Log.cpp)
    target_include_directories(lif_roundtrip_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    target_link_libraries(lif_roundtrip_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME lif_roundtrip COMMAND lif_roundtrip_test)
    message(STATUS "Tests enabled: lif_roundtrip_test")
endif()
