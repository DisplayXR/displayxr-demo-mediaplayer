#!/usr/bin/env bash
#
# scripts/build_linux.sh — Build the Linux demo binary (build-green, issue #30).
#
# Mirrors scripts/build_macos.sh per the runtime repo's
# docs/guides/linux-demo-port.md, with the Linux swaps: system Vulkan
# (libvulkan-dev — no MoltenVK, no ICD manifest), a from-source OpenXR loader
# pinned to the SAME release as CMakeLists.txt's FetchContent fallback, and no
# installer step (Linux packaging is out of scope until on-screen lands).
#
# Usage:
#   ./scripts/build_linux.sh
#
# Deps (Ubuntu): see .github/workflows/build-linux.yml — build-essential cmake
#   ninja-build pkg-config libvulkan-dev vulkan-validationlayers glslang-tools,
#   FFmpeg dev packages (libavformat-dev libavcodec-dev libavutil-dev
#   libswscale-dev libswresample-dev), and the X11/Wayland + ALSA dev headers
#   SDL3 needs when built from source.
#
# Env:
#   OPENXR_VERSION   OpenXR-SDK release tag for the loader (default 1.1.51).
#                    Keep this pin equal to CMakeLists.txt's FetchContent
#                    GIT_TAG — CI runs this script, so the CI pin follows.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# --- 0. Build OpenXR loader from source -----------------------------------
# Distro loaders lag; build the pinned Khronos loader and install it under
# /tmp/openxr-install (mirrors build_macos.sh + the runtime repo's
# scripts/build_linux.sh --apps). Cached: skipped if both the .so and the
# CMake package config are already present.
OPENXR_VERSION="${OPENXR_VERSION:-1.1.51}"
OPENXR_DIR="/tmp/openxr-install"
# The versioned .so is part of the cache key: a loader cached from an older pin
# would otherwise be linked (and bundled into the .deb) forever.
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.so.$OPENXR_VERSION" ] || \
   [ ! -f "$OPENXR_DIR/lib/cmake/openxr/OpenXRConfig.cmake" ]; then
    echo "==> Building OpenXR loader $OPENXR_VERSION -> $OPENXR_DIR"
    rm -rf /tmp/openxr-sdk "$OPENXR_DIR"
    git clone --depth 1 --branch "release-$OPENXR_VERSION" \
        https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk
    cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
        -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF
    cmake --build /tmp/openxr-sdk/build
    cmake --install /tmp/openxr-sdk/build
else
    echo "==> OpenXR loader cached at $OPENXR_DIR"
fi

# --- 1. cmake build -------------------------------------------------------
# Dev build: FFmpeg resolves via pkg-config; Vulkan via the system libvulkan-dev.
# The .deb (scripts/package_deb_linux.sh) instead sets FFMPEG_ROOT to the private
# slim FFmpeg + MP_LINUX_PORTABLE=ON, in its own BUILD_DIR (#76).
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
EXTRA_ARGS=()
[ -n "${FFMPEG_ROOT:-}" ] && EXTRA_ARGS+=("-DFFMPEG_ROOT=$FFMPEG_ROOT")
[ -n "${MP_LINUX_PORTABLE:-}" ] && EXTRA_ARGS+=("-DMP_LINUX_PORTABLE=$MP_LINUX_PORTABLE")
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR" \
    "${EXTRA_ARGS[@]}"
cmake --build "$BUILD_DIR"

BIN="$BUILD_DIR/mediaplayer_handle_vk_linux"
[ -x "$BIN" ] || { echo "Error: expected binary not found at $BIN" >&2; exit 1; }

echo ""
echo "Built: $BIN"
echo "Run against a dev runtime: scripts/run_mediaplayer_handle_vk_linux.sh"
