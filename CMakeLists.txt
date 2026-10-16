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
    enable_language(OBJCXX)   # displayxr-common's cursor_depth_macos.mm (see below)
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
        GIT_TAG release-1.1.51
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
#   * Linux portable .deb (MP_LINUX_PORTABLE, #76): a private slim FFmpeg built by
#     scripts/build_ffmpeg_linux.sh, passed as FFMPEG_ROOT and bundled next to the
#     binary — the distro libav* sonames differ on every Ubuntu release.
# An explicit FFMPEG_ROOT always wins over pkg-config, and is searched exclusively
# (NO_DEFAULT_PATH), so a system FFmpeg on the build host can never be linked instead.
find_package(PkgConfig QUIET)
set(MP_HAVE_FFMPEG FALSE)
set(MP_FFMPEG_LINK "")           # what the target links against (pkg-config target or libs)
set(MP_FFMPEG_DLL_DIR "")        # Windows: dir holding the runtime DLLs to stage by the exe
if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
    set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()
if(PkgConfig_FOUND AND NOT FFMPEG_ROOT)
    pkg_check_modules(FFMPEG IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    if(FFMPEG_FOUND)
        set(MP_HAVE_FFMPEG TRUE)
        set(MP_FFMPEG_LINK PkgConfig::FFMPEG)
        message(STATUS "FFmpeg: found via pkg-config (libavcodec ${FFMPEG_libavcodec_VERSION})")
    endif()
endif()
if(NOT MP_HAVE_FFMPEG)
    # Manual path. Honor FFMPEG_ROOT (cache or env) exclusively; without it, still
    # let find_*/CMAKE_PREFIX_PATH (e.g. a vcpkg toolchain) resolve the libs.
    set(_mp_ff_find_args)
    if(FFMPEG_ROOT)
        set(_mp_ff_find_args NO_DEFAULT_PATH)
    endif()
    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${FFMPEG_ROOT}/include" ${_mp_ff_find_args})
    foreach(_ff avcodec avformat avutil swscale swresample)
        find_library(FFMPEG_${_ff} ${_ff} HINTS "${FFMPEG_ROOT}/lib" ${_mp_ff_find_args})
    endforeach()
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

# --- SDL3 (window + input + native handle + audio out + open-file dialog). Pinned.
#   * Windows / macOS: uses an installed SDL3 if one is present, else builds the
#     pinned source as a shared lib (the installers stage SDL3.dll / the dylib).
#   * Linux: ALWAYS the pinned source, linked STATICALLY (#76). Ubuntu 24.04 ships
#     no libsdl3 package at all (the archive has it from 25.04), so a shared SDL3
#     can be neither bundled-by-distro nor declared in the .deb's Depends. Static
#     costs nothing at runtime: SDL still dlopen()s its X11/Wayland/PulseAudio/
#     PipeWire/ALSA backends (SDL_*_SHARED default ON), so the binary gains no
#     DT_NEEDED on any of them. Never falls back to a system SDL3 here, so a 26.04
#     build host with libsdl3-dev cannot silently reintroduce libSDL3.so.0.
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    # Subsystems the player never touches (it uses video, events, audio, dialog;
    # joystick/gamepad stay because the ImGui SDL3 backend polls gamepads).
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(_mp_sdl_find_args)
else()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(_mp_sdl_find_args FIND_PACKAGE_ARGS NAMES SDL3)
endif()
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.10
    GIT_SHALLOW TRUE
    ${_mp_sdl_find_args})
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
# --- displayxr-common: header-only dxr_view_config.h (DxrSelectViewConfigType +
#     DxrAliasInactiveViews, runtime ADR-041 / #1612) — the ONE shared copy every
#     demo uses. Sources only: common's own CMake carries desktop app scaffolding
#     this player does not use, so it is populated, not added. Keep this pin equal
#     to android/src/main/cpp/CMakeLists.txt's.
#
#     Desktop Linux is the exception: the player's WINDOW is displayxr::linux_window
#     (DxrLinuxWindow — the one Linux window every DisplayXR demo uses: X11 or
#     native Wayland by its capability probe, the client-side title bar, the
#     phase-snapped title-bar and right-button drags, F11 onto the panel, key
#     text, file drops, cursor shapes), so there common IS added, at the pin that
#     ships all of that. Windows / macOS keep the headers-only v2.15.0 populate.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/displayxr-openxr" CACHE PATH
        "DisplayXR OpenXR extension headers for displayxr-common")
    FetchContent_Declare(displayxr_common
        GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
        # v2.20.0 ships displayxr::linux_window.
        # v2.21.0 adds the native-Wayland drag-lattice follow-ups: a table when a
        # drag reaches the 3D panel, probing off the render thread, a per-drag
        # summary (displayxr-common#59, runtime#1697).
        # v2.22.0 declares an opaque window's Wayland surface opaque, which lets
        # the compositor scan a fullscreen weave out directly (displayxr-common#60,
        # runtime#1698).
        # v2.23.0: the native-Wayland drag stays phase-snapped at ANY output scale
        # (150 %, 166.67 %, ...), not only 100/200 % (displayxr-common#61,
        # runtime#1709).
        # v2.24.0: the Wayland drag sends the compositor move at once and builds the
        # first lattice table on a worker; an optional bulk snap-grid provider is
        # added and not used here (displayxr-common#62, #63, runtime#1609, #1723).
        # v2.25.0: the drag fixes: a dense Wayland drag-lattice table, no lattice
        # while the runtime move-syncs the window, a drag that ends on its button's
        # release, X11 drag steps that land exactly, and X11 header-bar double-click
        # maximise (displayxr-common#64-#68, runtime#1748).
        # v2.25.1: the Wayland surface scale is separate from the window-geometry
        # stage factor in mutter's PHYSICAL layout mode (displayxr-common#69).
        # v2.26.0: key text, file drops and app cursor shapes, which the player's
        # window needs now that it runs on DxrLinuxWindow (displayxr-common#70).
        # v2.28.0: dxr::VkFrameRing (common/vk_frame_ring.h) — N frames in flight,
        # one submit per frame (runtime INV-4.10, #1942). ONE pin on every leg now.
        # v2.30.1: the Wayland geometry payload is read by structure, in any key order
        # (displayxr-common#76; a sorted-key KWin publisher crashed the old parser).
        # v2.31.0: dxr::CursorDepth::useMeasuredDisparity(), the XR_DXR_cursor_depth v7
        # measured-disparity source (displayxr-common#77, runtime#1976).
        # v2.31.2: a right-button content drag runs on KWin (displayxr-common#78, only
        # gtk_shell1 says mutter) + the buffer_device helper in u_wayland_geom.h (#81).
        GIT_TAG v2.31.2
        GIT_SHALLOW TRUE)
else()
    FetchContent_Declare(displayxr_common
        GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
        # v2.30.1: the Wayland geometry payload is read by structure, in any key order
        # (displayxr-common#76; a sorted-key KWin publisher crashed the old parser).
        # v2.31.0: dxr::CursorDepth::useMeasuredDisparity(), the XR_DXR_cursor_depth v7
        # measured-disparity source (displayxr-common#77, runtime#1976).
        # v2.31.2: a right-button content drag runs on KWin (displayxr-common#78, only
        # gtk_shell1 says mutter) + the buffer_device helper in u_wayland_geom.h (#81).
        GIT_TAG v2.31.2
        GIT_SHALLOW TRUE
        # No CMakeLists.txt there: MakeAvailable only populates, never add_subdirectory
        # (the non-deprecated form of a bare FetchContent_Populate).
        SOURCE_SUBDIR _headers_only_no_cmake)
endif()
FetchContent_MakeAvailable(displayxr_common)

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
# ---------------------------------------------------------------------------
# Targets — ONE trunk, two products (docs/PRO.md).
#   mediaplayer_handle_vk_<plat>      the Media Player (demo bundle, versions.json)
#   mediaplayer_pro_handle_vk_<plat>  Media Player Pro: the same sources + src/pro/,
#                                     built with MEDIAPLAYER_PRO=1; own installer and
#                                     own release tags (pro-v*), NOT in the DisplayXR bundle.
# Every trunk change lands in both. Pro-only code lives in src/pro/ behind the
# `#if MEDIAPLAYER_PRO` hooks in App.cpp — never fork a trunk file for Pro.
# ---------------------------------------------------------------------------
option(MP_LINUX_PORTABLE "Linux: RUNPATH=\$ORIGIN + stage bundled libs next to the binary (for the .deb)" OFF)

function(mp_add_player tgt is_pro)
    add_executable(${tgt}
        src/main.cpp
        src/color/CubeLut.cpp    # Pro looks; compiled into both so the trunk stays one source list
        src/color/Cdl.cpp
        src/App.cpp
        src/Log.cpp
        src/platform/Window.cpp
        $<$<PLATFORM_ID:Linux>:src/platform/WindowLinux.cpp>  # desktop Linux: displayxr-common window
        src/xr/XrSession.cpp
        src/rhi/VulkanRenderer.cpp
        src/media/ImageDecoder.cpp
        src/media/LifLoader.cpp
        src/media/MpoLoader.cpp
        src/media/MediaSource.cpp
        src/media/StereoCompose.cpp
        src/media/StereoDetect.cpp
        src/media/VideoStereoProbe.cpp
        src/media/VideoDecoder.cpp
        src/media/AudioPlayer.cpp
        src/media/CameraSelect.cpp
        src/media/CameraSource.cpp   # live camera (#90): SDL3 camera -> FrameRing
        src/media/ImageSequence.cpp  # EXR / DPX / TIFF frames as one clip (Pro formats only)
        src/media/AutoConvergence.cpp  # live auto-convergence (#92): disparity.js port
        src/media/StreamUrl.cpp      # stream-URL classifier (#93)
        src/ui/Hud.cpp
        src/ui/ImGuiLayer.cpp
        # The player's ImGui chrome, shared VERBATIM with the Android leg. If you add or
        # split a TU here, mirror it into android/src/main/cpp/CMakeLists.txt — that list
        # is hand-curated and desktop CI cannot catch the resulting Android link error.
        src/ui/TransportUI.cpp
        src/media/FrameRing.h  # header-only triple buffer
        ${MP_IMGUI_SOURCES}
    )

    # Windows: link as a GUI (WINDOWS subsystem) app so launching the player never
    # pops a console window — neither standalone (Explorer) nor under the shell. SDL3's
    # <SDL3/SDL_main.h> supplies the WinMain entry that forwards to our main(), so the
    # console subsystem is unnecessary. stderr still works when a console is attached
    # (e.g. the shell's CREATE_NEW_CONSOLE) or redirected.
    if(WIN32)
        set_target_properties(${tgt} PROPERTIES WIN32_EXECUTABLE TRUE)
        # Embedded Explorer icon + version info (icon from the 2D workspace logo).
        target_sources(${tgt} PRIVATE windows/resource.rc)
    endif()

    target_include_directories(${tgt} PRIVATE
        ${MP_IMGUI_DIR} ${MP_IMGUI_DIR}/backends)
    # MEDIAPLAYER_IMGUI_SDL selects the SDL3 ImGui *platform* backend (input + dt) on
    # Windows / macOS. Desktop Linux has no SDL window: MEDIAPLAYER_IMGUI_DXRWIN feeds
    # ImGui from displayxr-common's window events instead. The Android leg defines
    # MEDIAPLAYER_WITH_IMGUI only: it has no SDL, and feeds input through
    # ImGuiLayer::PushPointer() instead. See src/ui/ImGuiLayer.h.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(${tgt} PRIVATE MEDIAPLAYER_WITH_IMGUI=1 MEDIAPLAYER_IMGUI_DXRWIN=1)
    else()
        target_compile_definitions(${tgt} PRIVATE MEDIAPLAYER_WITH_IMGUI=1 MEDIAPLAYER_IMGUI_SDL=1)
    endif()

    if(MP_HAVE_FFMPEG)
        target_compile_definitions(${tgt} PRIVATE MEDIAPLAYER_WITH_FFMPEG=1)
        target_link_libraries(${tgt} PRIVATE ${MP_FFMPEG_LINK})
        # Windows shared FFmpeg: copy the runtime DLLs next to the binary post-build.
        if(MP_FFMPEG_DLL_DIR)
            file(GLOB _ffmpeg_dlls "${MP_FFMPEG_DLL_DIR}/*.dll")
            foreach(_dll ${_ffmpeg_dlls})
                add_custom_command(TARGET ${tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_dll}" "$<TARGET_FILE_DIR:${tgt}>"
                    COMMENT "Staging FFmpeg DLL $<PATH:GET_FILENAME,${_dll}> next to ${tgt}")
            endforeach()
        endif()
    endif()

    target_include_directories(${tgt} BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/displayxr-openxr  # vendored <openxr/...>
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb               # stb_image_write.h
    )
    # displayxr-common's header-only rules (dxr_view_config.h, vk_frame_ring.h) come
    # through displayxr::rules. A linked target's include dirs land AFTER this target's
    # own (the BEFORE block above), so common only supplies what this repo does not.
    # Desktop Linux add_subdirectory's common and gets the real target; the
    # headers-only populate (Windows / macOS) gets an equivalent local one.
    if(NOT TARGET displayxr::rules)
        add_library(mp_displayxr_rules INTERFACE)
        target_include_directories(mp_displayxr_rules INTERFACE ${displayxr_common_SOURCE_DIR}/common)
        add_library(displayxr::rules ALIAS mp_displayxr_rules)
    endif()
    target_link_libraries(${tgt} PRIVATE displayxr::rules)

    target_link_libraries(${tgt} PRIVATE
        SDL3::SDL3
        Vulkan::Vulkan
        nlohmann_json::nlohmann_json
        ${MP_OPENXR_LOADER}
    )
    # Desktop Linux: the app window (X11 or native Wayland) — see the fetch above.
    if(TARGET displayxr::linux_window)
        target_link_libraries(${tgt} PRIVATE displayxr::linux_window)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "displayxr::linux_window is not available — install libx11-dev "
                            "(and libwayland-dev libxkbcommon-dev libdbus-1-dev)")
    endif()

    # Windows: SHGetKnownFolderPath (shell32) + CoTaskMemFree (ole32) for the 'I'-key
    # atlas-capture output path under the user's Pictures folder.
    # d3d11 + dxgi: zero-copy D3D11VA->Vulkan decode interop (issue #28).
    if(WIN32)
        target_link_libraries(${tgt} PRIVATE shell32 ole32 d3d11 dxgi d3dcompiler)
    endif()

    # macOS: displayxr-common's Cocoa adapter for dxr::CursorDepth hides the OS pointer
    # with +[NSCursor hide] while the runtime draws the depth cursor. SDL_HideCursor would
    # not do: SDL swaps in an INVISIBLE NSCursor, and the runtime's default cursor look
    # mirrors +[NSCursor currentCursor] — it would draw that invisible sprite.
    if(APPLE)
        target_sources(${tgt} PRIVATE ${displayxr_common_SOURCE_DIR}/common/cursor_depth_macos.mm)
        target_link_libraries(${tgt} PRIVATE "-framework AppKit")
    endif()

    # Copy the OpenXR loader dylib/dll next to the binary when it's an imported target,
    # so the app runs without extra PATH/DYLD setup.
    if(TARGET OpenXR::openxr_loader)
        get_target_property(_oxr_loc OpenXR::openxr_loader IMPORTED_LOCATION)
        if(NOT _oxr_loc)
            get_target_property(_oxr_loc OpenXR::openxr_loader IMPORTED_LOCATION_RELEASE)
        endif()
        if(_oxr_loc)
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_oxr_loc}" "$<TARGET_FILE_DIR:${tgt}>"
                COMMENT "Copying OpenXR loader next to ${tgt}")
        endif()
    endif()

    # Linux portable build (the .deb, #76): the binary finds its bundled libs (the
    # private FFmpeg, the OpenXR loader) in its own directory via RUNPATH=$ORIGIN — no
    # build-tree path baked in, no LD_LIBRARY_PATH needed. The same libs are staged
    # next to the binary here so the build tree runs exactly like the installed layout.
    if(MP_LINUX_PORTABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(NOT FFMPEG_ROOT)
            message(FATAL_ERROR "MP_LINUX_PORTABLE needs FFMPEG_ROOT (scripts/build_ffmpeg_linux.sh): "
                                "a distro FFmpeg cannot ship in a portable .deb")
        endif()
        set_target_properties(${tgt} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "\$ORIGIN")
        file(GLOB _mp_bundled_libs "${FFMPEG_ROOT}/lib/lib*.so.*")
        if(TARGET OpenXR::openxr_loader AND _oxr_loc)
            get_filename_component(_oxr_dir "${_oxr_loc}" DIRECTORY)
            file(GLOB _oxr_libs "${_oxr_dir}/libopenxr_loader.so*")
            list(APPEND _mp_bundled_libs ${_oxr_libs})
        endif()
        foreach(_lib ${_mp_bundled_libs})
            # -P keeps soname symlinks as symlinks.
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND cp -P -f "${_lib}" "$<TARGET_FILE_DIR:${tgt}>/"
                VERBATIM)
        endforeach()
        message(STATUS "MP_LINUX_PORTABLE: RUNPATH=$ORIGIN, bundling ${FFMPEG_ROOT}/lib + OpenXR loader")
    endif()

    # Stage dependent shared-library DLLs (SDL3 when built shared, an OpenXR loader DLL
    # if the loader is dynamic, etc.) next to the binary so it runs in-place on Windows
    # without PATH munging. FFmpeg's DLLs come via MP_FFMPEG_DLL_DIR above (raw .lib
    # paths, not CMake targets, so they're not in TARGET_RUNTIME_DLLS).
    if(WIN32)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${tgt}> "$<TARGET_FILE_DIR:${tgt}>"
            COMMAND_EXPAND_LISTS
            COMMENT "Staging dependent runtime DLLs next to ${tgt}")
    endif()

    # Sidecar workspace manifest + real logos (generated by /make-app-logos from a
    # live atlas capture). Staged next to the binary on every platform so dev builds
    # and the installer's BIN_DIR both carry them, and the idle logo resolves at
    # <exe-dir>/displayxr/ (manifest spec) instead of falling back to the test pattern.
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/displayxr" "$<TARGET_FILE_DIR:${tgt}>/displayxr"
        COMMENT "Staging displayxr/ sidecar (manifest + logos) next to ${tgt}")
    if(is_pro)
        target_sources(${tgt} PRIVATE
            src/pro/ProController.cpp
            src/pro/ProTools.cpp         # agent tools: grade, view, station
            src/pro/LivePixels.cpp       # UYVY / v210 / BGRA -> NV12 / P016 for capture cards
            src/pro/LiveSourceBase.cpp   # shared ring + stats of the Pro live sources
            src/pro/TestSignalSource.cpp # synthetic stereo rig (no hardware)
            src/pro/TimecodePairer.cpp   # two-input stereo pairing by SMPTE timecode
            src/pro/DeckLinkSource.cpp   # Blackmagic DeckLink SDI / HDMI input
            src/pro/RigCheck.cpp         # vertical / depth / colour checks + 60 s history
            src/pro/Station.cpp          # inputs, timecode / delay, rig UI, snapshots
            src/pro/OcioBake.cpp)        # OpenColorIO -> the grade's LUT (MP_OCIO_ROOT)
        target_compile_definitions(${tgt} PRIVATE MEDIAPLAYER_PRO=1)
        if(MP_HAVE_OCIO)
            target_link_libraries(${tgt} PRIVATE OpenColorIO::OpenColorIO)
            target_compile_definitions(${tgt} PRIVATE MEDIAPLAYER_WITH_OCIO=1)
            foreach(_d ${MP_OCIO_DLLS})
                add_custom_command(TARGET ${tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_d}" "$<TARGET_FILE_DIR:${tgt}>")
            endforeach()
        endif()
        if(MP_DECKLINK_INCLUDE)
            target_sources(${tgt} PRIVATE "${MP_DECKLINK_INCLUDE}/DeckLinkAPIDispatch.cpp")
            target_include_directories(${tgt} PRIVATE "${MP_DECKLINK_INCLUDE}")
            target_compile_definitions(${tgt} PRIVATE MEDIAPLAYER_WITH_DECKLINK=1)
        endif()
    else()
        target_compile_definitions(${tgt} PRIVATE MEDIAPLAYER_PRO=0)
    endif()
    message(STATUS "Configured target: ${tgt}")
endfunction()

# Blackmagic DeckLink input (Pro, docs/PRO.md Next 1). The Desktop Video SDK is a
# registration download and is never committed: point MP_DECKLINK_SDK_DIR (or the env
# var of the same name) at the SDK zip or its extracted folder. The COM headers are
# generated from its IDL with midl at configure time, as Open RV does. Without it Pro
# builds and its DeckLink input reports "no DeckLink support".
set(MP_DECKLINK_SDK_DIR "$ENV{MP_DECKLINK_SDK_DIR}" CACHE PATH "Blackmagic DeckLink SDK zip or folder (Pro, Windows)")
set(MP_DECKLINK_INCLUDE "")
if(WIN32 AND MP_DECKLINK_SDK_DIR)
    set(_dl_root "${MP_DECKLINK_SDK_DIR}")
    if(_dl_root MATCHES "\.zip$")
        set(_dl_extract "${CMAKE_BINARY_DIR}/decklink_sdk")
        if(NOT EXISTS "${_dl_extract}")
            file(ARCHIVE_EXTRACT INPUT "${_dl_root}" DESTINATION "${_dl_extract}")
        endif()
        set(_dl_root "${_dl_extract}")
    endif()
    file(GLOB_RECURSE _dl_idl "${_dl_root}/*/Win/include/DeckLinkAPI.idl" "${_dl_root}/Win/include/DeckLinkAPI.idl")
    list(LENGTH _dl_idl _dl_n)
    if(_dl_n EQUAL 0)
        message(WARNING "MP_DECKLINK_SDK_DIR=${MP_DECKLINK_SDK_DIR}: no Win/include/DeckLinkAPI.idl; DeckLink off")
    else()
        list(GET _dl_idl 0 _dl_idl)
        get_filename_component(_dl_inc "${_dl_idl}" DIRECTORY)
        set(_dl_gen "${CMAKE_BINARY_DIR}/decklink_gen")
        if(NOT EXISTS "${_dl_gen}/DeckLinkAPI.h")
            file(MAKE_DIRECTORY "${_dl_gen}")
            execute_process(
                COMMAND midl /nologo /header DeckLinkAPI.h /iid DeckLinkAPIDispatch.cpp /out "${_dl_gen}" DeckLinkAPI.idl
                WORKING_DIRECTORY "${_dl_inc}" RESULT_VARIABLE _dl_midl OUTPUT_VARIABLE _dl_out ERROR_VARIABLE _dl_out)
            if(NOT _dl_midl EQUAL 0)
                message(WARNING "midl failed on the DeckLink IDL (${_dl_midl}): ${_dl_out}")
            endif()
        endif()
        if(EXISTS "${_dl_gen}/DeckLinkAPI.h")
            set(MP_DECKLINK_INCLUDE "${_dl_gen}")
            message(STATUS "DeckLink input (Pro): ON (${_dl_inc})")
        endif()
    endif()
else()
    message(STATUS "DeckLink input (Pro): OFF (set MP_DECKLINK_SDK_DIR to the Desktop Video SDK)")
endif()

# OpenColorIO (Pro, optional): an OCIO 2.x install prefix (e.g. Open RV's build,
# <OpenRV>/_build/RV_DEPS_OCIO/install) and, on Windows, the folder holding its runtime
# DLLs (Imath, expat, zlib: <OpenRV>/_build/stage/app/bin). Off by default.
set(MP_OCIO_ROOT "" CACHE PATH "OpenColorIO 2.x install prefix (Pro, optional)")
set(MP_OCIO_DLL_DIR "" CACHE PATH "Folder with OpenColorIO's dependency DLLs (Windows)")
set(MP_HAVE_OCIO FALSE)
set(MP_OCIO_DLLS "")
if(MP_OCIO_ROOT)
    find_package(OpenColorIO CONFIG PATHS "${MP_OCIO_ROOT}" NO_DEFAULT_PATH)
    if(OpenColorIO_FOUND)
        set(MP_HAVE_OCIO TRUE)
        if(WIN32)
            file(GLOB MP_OCIO_DLLS "${MP_OCIO_ROOT}/bin/OpenColorIO*.dll")
            if(MP_OCIO_DLL_DIR)
                file(GLOB _ocio_dep "${MP_OCIO_DLL_DIR}/Imath*.dll" "${MP_OCIO_DLL_DIR}/libexpat*.dll"
                                    "${MP_OCIO_DLL_DIR}/zlib*.dll")
                list(APPEND MP_OCIO_DLLS ${_ocio_dep})
            endif()
        endif()
        message(STATUS "OpenColorIO (Pro): ON (${OpenColorIO_VERSION})")
    else()
        message(WARNING "MP_OCIO_ROOT=${MP_OCIO_ROOT}: no OpenColorIO package found; OCIO off")
    endif()
endif()

mp_add_player(${MP_TARGET} OFF)

# Media Player Pro builds on Windows first (the 3D-panel monitoring station); the
# other platforms can opt in with -DMP_BUILD_PRO=ON once they are validated.
if(WIN32)
    set(_mp_pro_default ON)
else()
    set(_mp_pro_default OFF)
endif()
option(MP_BUILD_PRO "Also build Media Player Pro (mediaplayer_pro_handle_vk_<plat>)" ${_mp_pro_default})
set(MP_PRO_TARGET "mediaplayer_pro_handle_vk_${MP_PLATFORM}")
if(MP_BUILD_PRO)
    mp_add_player(${MP_PRO_TARGET} ON)
    # Both targets stage DLLs + the displayxr/ sidecar into the same build dir in their
    # POST_BUILD steps; serialise them so the two copies never race.
    add_dependencies(${MP_PRO_TARGET} ${MP_TARGET})
endif()


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
        src/media/StereoCompose.cpp
        src/media/ImageDecoder.cpp
        src/Log.cpp)
    target_include_directories(lif_roundtrip_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    target_link_libraries(lif_roundtrip_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME lif_roundtrip COMMAND lif_roundtrip_test)

    # The stereo detector is pure by construction — no Vulkan, no SDL, no libav — so it
    # links against almost nothing and runs on synthetic buffers with no fixtures.
    add_executable(stereo_detect_test
        tests/stereo_detect_test.cpp
        src/media/StereoDetect.cpp
        src/media/MediaSource.cpp
        src/Log.cpp)
    target_include_directories(stereo_detect_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME stereo_detect COMMAND stereo_detect_test)

    # Live camera source (#90): denylist / selector / format choice. Pure, no SDL.
    add_executable(camera_select_test
        tests/camera_select_test.cpp
        src/media/CameraSelect.cpp
        src/Log.cpp)
    target_include_directories(camera_select_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME camera_select COMMAND camera_select_test)

    # Auto-convergence (#92): synthetic SBS pairs with known disparity. Pure, no SDL.
    add_executable(auto_convergence_test
        tests/auto_convergence_test.cpp
        src/media/AutoConvergence.cpp)
    target_include_directories(auto_convergence_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME auto_convergence COMMAND auto_convergence_test)

    # Stream-URL source (#93): the scheme classifier / host / redaction. Pure, no FFmpeg.
    add_executable(stream_url_test
        tests/stream_url_test.cpp
        src/media/StreamUrl.cpp)
    target_include_directories(stream_url_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME stream_url COMMAND stream_url_test)

    # Media Player Pro looks: .cube LUT + ASC CDL loaders (pure C++).
    # Pro live sources, two-input pairing and rig checks (src/pro/, pure: no SDK, no GPU).
    add_executable(live_pixels_test tests/live_pixels_test.cpp src/pro/LivePixels.cpp
        src/pro/LiveSourceBase.cpp src/pro/TestSignalSource.cpp src/media/ImageDecoder.cpp src/Log.cpp)
    target_include_directories(live_pixels_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    add_test(NAME live_pixels COMMAND live_pixels_test)
    add_executable(timecode_pairer_test tests/timecode_pairer_test.cpp src/pro/TimecodePairer.cpp)
    target_include_directories(timecode_pairer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME timecode_pairer COMMAND timecode_pairer_test)
    add_executable(rig_check_test tests/rig_check_test.cpp src/pro/RigCheck.cpp src/pro/LivePixels.cpp
        src/pro/LiveSourceBase.cpp src/pro/TestSignalSource.cpp src/media/ImageDecoder.cpp src/Log.cpp)
    target_include_directories(rig_check_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    add_test(NAME rig_check COMMAND rig_check_test)

    # Pro review formats: sequences, EXR display encoding, 10-bit -> P016 (real decodes).
    add_executable(image_sequence_test tests/image_sequence_test.cpp src/media/ImageSequence.cpp)
    target_include_directories(image_sequence_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME image_sequence COMMAND image_sequence_test)
    if(TARGET PkgConfig::FFMPEG OR TARGET mp_ffmpeg)
        add_executable(decode_formats_test tests/decode_formats_test.cpp src/media/VideoDecoder.cpp
            src/media/ImageSequence.cpp src/Log.cpp)
        target_include_directories(decode_formats_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(decode_formats_test PRIVATE MEDIAPLAYER_WITH_FFMPEG=1)
        if(TARGET PkgConfig::FFMPEG)
            target_link_libraries(decode_formats_test PRIVATE PkgConfig::FFMPEG)
        else()
            target_link_libraries(decode_formats_test PRIVATE mp_ffmpeg)
        endif()
        if(WIN32)
            target_link_libraries(decode_formats_test PRIVATE d3d11 dxgi d3dcompiler)
        endif()
        add_test(NAME decode_formats COMMAND decode_formats_test)
    endif()
    # OpenColorIO in the grade (Pro): skips cleanly without OCIO.
    add_executable(ocio_bake_test tests/ocio_bake_test.cpp src/pro/OcioBake.cpp)
    target_include_directories(ocio_bake_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MP_HAVE_OCIO)
        target_link_libraries(ocio_bake_test PRIVATE OpenColorIO::OpenColorIO)
        target_compile_definitions(ocio_bake_test PRIVATE MEDIAPLAYER_WITH_OCIO=1)
        if(MP_BUILD_PRO)
            add_dependencies(ocio_bake_test ${MP_PRO_TARGET})  # the Pro build copies the DLLs
        endif()
    endif()
    add_test(NAME ocio_bake COMMAND ocio_bake_test)

    # The decoder drains at EOF (short clips, last frames): writes a 5-frame ProRes clip.
    if(TARGET PkgConfig::FFMPEG OR TARGET mp_ffmpeg)
        add_executable(short_clip_test tests/short_clip_test.cpp src/media/VideoDecoder.cpp
            src/media/ImageSequence.cpp src/Log.cpp)
        target_include_directories(short_clip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(short_clip_test PRIVATE MEDIAPLAYER_WITH_FFMPEG=1)
        if(TARGET PkgConfig::FFMPEG)
            target_link_libraries(short_clip_test PRIVATE PkgConfig::FFMPEG)
        else()
            target_link_libraries(short_clip_test PRIVATE mp_ffmpeg)
        endif()
        if(WIN32)
            target_link_libraries(short_clip_test PRIVATE d3d11 dxgi d3dcompiler)
        endif()
        add_test(NAME short_clip COMMAND short_clip_test)
    endif()

    # The Resolve marker script (tools/resolve) against a fake timeline; needs Python 3.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME resolve_markers
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/resolve_markers_test.py)
    endif()

    add_executable(color_test
        tests/color_test.cpp
        src/color/CubeLut.cpp
        src/color/Cdl.cpp)
    target_include_directories(color_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME color COMMAND color_test)

    add_executable(mpo_parse_test
        tests/mpo_parse_test.cpp
        src/media/MpoLoader.cpp
        src/media/StereoCompose.cpp
        src/media/ImageDecoder.cpp
        src/Log.cpp)
    target_include_directories(mpo_parse_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    add_test(NAME mpo_parse COMMAND mpo_parse_test)

    # End-to-end layout sweep over the generated corpus (scripts/gen_stereo_test_assets.sh).
    # Needs the media stack but no Vulkan/OpenXR/display; skips cleanly (exit 0) when the
    # asset directory is absent, so CI passes without generating gigabytes of fixtures.
    add_executable(stereo_sweep
        tests/stereo_sweep.cpp
        src/media/MediaSource.cpp
        src/media/StereoDetect.cpp
        src/media/StereoCompose.cpp
        src/media/VideoStereoProbe.cpp
        src/media/LifLoader.cpp
        src/media/MpoLoader.cpp
        src/media/ImageDecoder.cpp
        src/Log.cpp)
    target_include_directories(stereo_sweep PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    target_link_libraries(stereo_sweep PRIVATE nlohmann_json::nlohmann_json)
    if(TARGET PkgConfig::FFMPEG)
        target_link_libraries(stereo_sweep PRIVATE PkgConfig::FFMPEG)
        target_compile_definitions(stereo_sweep PRIVATE MEDIAPLAYER_WITH_FFMPEG=1)
    elseif(TARGET mp_ffmpeg)
        target_link_libraries(stereo_sweep PRIVATE mp_ffmpeg)
        target_compile_definitions(stereo_sweep PRIVATE MEDIAPLAYER_WITH_FFMPEG=1)
    endif()
    add_test(NAME stereo_sweep COMMAND stereo_sweep
             ${CMAKE_CURRENT_SOURCE_DIR}/assets/media/detect)

    message(STATUS
        "Tests enabled: lif_roundtrip_test stereo_detect_test mpo_parse_test camera_select_test auto_convergence_test stream_url_test stereo_sweep")
endif()
