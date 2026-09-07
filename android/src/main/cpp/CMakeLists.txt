# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Native build for the Android leg of displayxr-demo-mediaplayer (stereo SBS
# image/video player). Reuses the cube_handle/model_viewer OpenXR-Android
# harness and binds to the DisplayXR runtime out-of-process (ADR-025). The
# renderer is a minimal SBS blit (fullscreen.vert + sbs.frag), one UV half per
# eye; video decodes via AMediaCodec (libmediandk) into the shader's YUV path.
#
# The SBS shaders are the SHARED repo-root shaders/, referenced by relative
# path — NOT copied — so the Android leg and the desktop legs use the exact
# same blit. The transport-overlay shaders are Android-local (the desktop UI
# is Dear ImGui). Audio (AAudio) is Stage 3 and not built.

cmake_minimum_required(VERSION 3.22.1)
project(mediaplayer_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root is four levels up from android/src/main/cpp.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# ── nlohmann/json (header-only) for the SHARED LifLoader's container JSON ──
# Same tag as the desktop build (top-level CMakeLists.txt).
include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# ── Dear ImGui — SAME TAG as the desktop build (top-level CMakeLists.txt) ──
# The player's chrome is shared verbatim with the desktop legs (src/ui/TransportUI.cpp),
# so the two can't drift. Backends: imgui_impl_vulkan ONLY.
#   NOT imgui_impl_sdl3    — there is no SDL on Android.
#   NOT imgui_impl_android — it reads AInputEvent from the NativeActivity input queue,
#                            which this app never receives (the runtime's overlay covers
#                            the window; touch arrives via dispatchTouchEvent -> JNI).
#                            Input is fed by hand through ImGuiLayer::PushPointer().
# No font asset: ImGui's embedded font is what the desktop uses too (only FontGlobalScale
# is set), so typography matches with zero packaging changes.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
set(MP_IMGUI_DIR "${imgui_SOURCE_DIR}")
set(MP_IMGUI_SOURCES
    ${MP_IMGUI_DIR}/imgui.cpp
    ${MP_IMGUI_DIR}/imgui_draw.cpp
    ${MP_IMGUI_DIR}/imgui_tables.cpp
    ${MP_IMGUI_DIR}/imgui_widgets.cpp
    ${MP_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)

# ── SBS blit + overlay shaders → SPIR-V header arrays ─────────────────────
# var name = <name>_<stage>_data, header = <name>.<stage>.h (matches the
# #includes in sbs_renderer.cpp). fullscreen.vert + sbs.frag come from the
# SHARED repo-root shaders/; overlay.* are Android-local.
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK on the build host")
endif()
set(SHADER_HEADERS "")
function(mp_compile_shader DIR S)
    string(REPLACE "." "_" VN "${S}")          # fullscreen.vert -> fullscreen_vert
    set(HDR "${CMAKE_CURRENT_BINARY_DIR}/${S}.h")
    add_custom_command(
        OUTPUT ${HDR}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2
            ${DIR}/${S} --vn ${VN}_data -o ${HDR}
        DEPENDS ${DIR}/${S}
        COMMENT "Compiling SPIR-V: ${S}")
    set(SHADER_HEADERS ${SHADER_HEADERS} ${HDR} PARENT_SCOPE)
endfunction()
mp_compile_shader("${REPO_ROOT}/shaders" fullscreen.vert)
mp_compile_shader("${REPO_ROOT}/shaders" sbs.frag)
mp_compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders" sbs_ahb.frag)  # zero-copy AHB ycbcr blit
mp_compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders" overlay.vert)
mp_compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders" overlay.frag)

# ── NativeActivity glue (ships with the NDK) ─────────────────────────────
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

add_library(mediaplayer_vk_android SHARED
    main.cpp
    sbs_renderer.cpp
    video_decoder.cpp
    audio_player.cpp
    lif_log_android.cpp
    # ── SHARED desktop sources (referenced, not copied): LIF container parse +
    #    stb image decode. Log.h's impl is the logcat shim above, NOT src/Log.cpp.
    #    ImageDecoder.cpp also owns the STB_IMAGE_IMPLEMENTATION TU.
    #    StereoCompose.cpp holds ComposeSbs + EstimateAutoConvergence, which LifLoader
    #    calls; they were factored out of LifLoader.cpp when MPO started sharing them
    #    (#45), so this target needs the extra TU or the link fails on both symbols.
    #
    #    ***THE RULE, GENERALLY***: this list names files individually and does NOT glob.
    #    Add or split ANY .cpp under ${REPO_ROOT}/src that this target uses and you must
    #    add it here too, or the Android link fails with `undefined symbol` while every
    #    desktop CI job stays green (they compile whole directories). #45 is the scar.
    # Stereo-layout resolution, SHARED with the desktop (#45 layered detector):
    # filename > container metadata > content cross-correlation > aspect. Pure
    # C++ (Log.h + std only). The content layer's frames come from
    # video_stereo_probe_android.cpp on this platform (CPU decode of the first
    # frames; the playback path is zero-copy and has no CPU pixels).
    ${REPO_ROOT}/src/media/MediaSource.cpp
    ${REPO_ROOT}/src/media/StereoDetect.cpp
    video_stereo_probe_android.cpp
    ${REPO_ROOT}/src/media/LifLoader.cpp
    ${REPO_ROOT}/src/media/StereoCompose.cpp
    ${REPO_ROOT}/src/media/ImageDecoder.cpp
    # The player's Dear ImGui chrome, SHARED VERBATIM with the desktop legs. This is
    # what makes the Android UI the same UI rather than a lookalike. ImGuiLayer builds
    # here WITHOUT MEDIAPLAYER_IMGUI_SDL, so its SDL3 platform backend compiles out and
    # input is fed through ImGuiLayer::PushPointer().
    ${REPO_ROOT}/src/ui/TransportUI.cpp
    ${REPO_ROOT}/src/ui/ImGuiLayer.cpp
    ${MP_IMGUI_SOURCES}
    ${SHADER_HEADERS})

target_include_directories(mediaplayer_vk_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated *.vert.h / *.frag.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb             # vendored stb_image.h (SBS image decode)
    ${REPO_ROOT}/src                            # Log.h (impl = lif_log_android.cpp)
    ${REPO_ROOT}/src/media                      # LifLoader.h, ImageDecoder.h, MediaSource.h
    ${REPO_ROOT}/third_party/displayxr-openxr  # DisplayXR XR_EXT_* headers
    ${MP_IMGUI_DIR} ${MP_IMGUI_DIR}/backends)  # Dear ImGui + the Vulkan backend

set_target_properties(mediaplayer_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

# WITH_IMGUI but deliberately NOT IMGUI_SDL — see the ImGui block above.
target_compile_definitions(mediaplayer_vk_android PRIVATE MEDIAPLAYER_WITH_IMGUI=1)

target_link_libraries(mediaplayer_vk_android
    native_app_glue
    OpenXR::openxr_loader
    nlohmann_json::nlohmann_json
    vulkan
    mediandk   # AMediaExtractor + AMediaCodec (framework-native video + audio decode)
    aaudio     # audio output (AudioPlayer → A/V master clock)
    android
    log)
