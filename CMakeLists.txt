# SPDX-License-Identifier: BSL-1.0
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
    FetchContent_Declare(OpenXR
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.43
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(OpenXR)
    set(MP_OPENXR_LOADER openxr_loader)
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
    # Header-only stubs (PRD module map; fleshed out in M1+):
    src/media/MediaSource.h
    src/media/VideoDecoder.h
    src/media/ImageDecoder.h
    src/media/FrameRing.h
    src/ui/Ui.h
)

target_include_directories(${MP_TARGET} BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/displayxr-openxr  # vendored <openxr/...>
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb               # stb_image_write.h
)

target_link_libraries(${MP_TARGET} PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    ${MP_OPENXR_LOADER}
)

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

message(STATUS "Configured target: ${MP_TARGET}")
