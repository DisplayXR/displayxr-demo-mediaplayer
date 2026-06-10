# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Native build for the Android leg of displayxr-demo-mediaplayer (stereo SBS
# image + video player). A vendor-neutral OpenXR-Android client (loader →
# instance → Vulkan device → per-eye swapchains → session) that binds to the
# DisplayXR runtime OUT-OF-PROCESS (ADR-025) — the runtime's vendor DP weaves.
#
# The SBS blit reuses the repo's SHARED shaders (../../../../shaders/sbs.frag +
# fullscreen.vert) — the exact shaders the desktop legs use. The transport
# overlay (triangle pipeline) is Android-specific (desktop uses ImGui), so its
# shaders live beside this CMakeLists. Decode is Android-native: AMediaCodec
# (video, libmediandk) + AAudio (audio) — FFmpeg isn't available on the NDK.

cmake_minimum_required(VERSION 3.22.1)
project(mediaplayer_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root is four levels up from android/src/main/cpp.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# ── Shaders → SPIR-V header arrays. var name = <name>_<stage>_data, header =
#    <name>.<stage>.h (matches the #includes in sbs_renderer.cpp). The SBS blit
#    shaders come from the repo-root shaders/ (shared with desktop); the
#    transport-overlay shaders are Android-leg-local. ───────────────────────
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK on the build host")
endif()

set(SHADER_HEADERS "")
function(compile_shader SRC_DIR S)
    string(REPLACE "." "_" VN "${S}")          # fullscreen.vert -> fullscreen_vert
    set(HDR "${CMAKE_CURRENT_BINARY_DIR}/${S}.h")
    add_custom_command(
        OUTPUT ${HDR}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2
            ${SRC_DIR}/${S} --vn ${VN}_data -o ${HDR}
        DEPENDS ${SRC_DIR}/${S}
        COMMENT "Compiling SPIR-V: ${S}")
    set(SHADER_HEADERS ${SHADER_HEADERS} ${HDR} PARENT_SCOPE)
endfunction()

compile_shader("${REPO_ROOT}/shaders" fullscreen.vert)
compile_shader("${REPO_ROOT}/shaders" sbs.frag)
compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders" overlay.vert)
compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders" overlay.frag)

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
    stb_impl.cpp
    ${SHADER_HEADERS})

target_include_directories(mediaplayer_vk_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated *.vert.h / *.frag.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb             # vendored stb_image.h (SBS image decode)
    ${REPO_ROOT}/third_party/displayxr-openxr)  # DisplayXR XR_EXT_display_info.h

set_target_properties(mediaplayer_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(mediaplayer_vk_android
    native_app_glue
    OpenXR::openxr_loader
    vulkan
    mediandk   # AMediaExtractor + AMediaCodec (framework-native video decode)
    aaudio     # AAudio output (audio playback / A-V master clock)
    android
    log)
