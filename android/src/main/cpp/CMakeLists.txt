# Copyright 2026, Leia Inc.
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
    ${REPO_ROOT}/src/media/LifLoader.cpp
    ${REPO_ROOT}/src/media/ImageDecoder.cpp
    ${SHADER_HEADERS})

target_include_directories(mediaplayer_vk_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated *.vert.h / *.frag.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb             # vendored stb_image.h (SBS image decode)
    ${REPO_ROOT}/src                            # Log.h (impl = lif_log_android.cpp)
    ${REPO_ROOT}/src/media                      # LifLoader.h, ImageDecoder.h, MediaSource.h
    ${REPO_ROOT}/third_party/displayxr-openxr)  # DisplayXR XR_EXT_* headers

set_target_properties(mediaplayer_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(mediaplayer_vk_android
    native_app_glue
    OpenXR::openxr_loader
    nlohmann_json::nlohmann_json
    vulkan
    mediandk   # AMediaExtractor + AMediaCodec (framework-native video + audio decode)
    aaudio     # audio output (AudioPlayer → A/V master clock)
    android
    log)
