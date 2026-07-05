#!/usr/bin/env bash
#
# scripts/build_macos.sh — Build the demo binary, and optionally the .pkg
# installer for distribution.
#
# Usage:
#   ./scripts/build_macos.sh                run cmake build only
#   ./scripts/build_macos.sh --installer    also build _package/DisplayXRMediaPlayer-*.pkg
#
# Env:
#   DISPLAYXR_VERSION   version string baked into the .pkg + .app Info.plist
#                       (defaults to 0.0.0-dev). CI sets this from the v* git tag.
#   OPENXR_VERSION      OpenXR-SDK release tag for the loader (default 1.1.51).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_INSTALLER=false
for arg in "${@}"; do
    case "$arg" in
        --installer) BUILD_INSTALLER=true ;;
        -h|--help) sed -n '1,16p' "$0"; exit 0 ;;
        *) echo "Unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# --- 0. Build OpenXR loader from source -----------------------------------
# Homebrew has no openxr-loader formula; build the Khronos loader and install
# it under /tmp/openxr-install (mirrors the runtime repo + sibling demos).
OPENXR_VERSION="${OPENXR_VERSION:-1.1.51}"
OPENXR_DIR="/tmp/openxr-install"
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.dylib" ]; then
    echo "==> Building OpenXR loader $OPENXR_VERSION -> $OPENXR_DIR"
    rm -rf /tmp/openxr-sdk
    git clone --depth 1 --branch "release-$OPENXR_VERSION" \
        https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk
    /usr/bin/sed -i '' \
        's|PRIVATE "${PROJECT_SOURCE_DIR}/src/external/jsoncpp/include"|BEFORE PRIVATE "${PROJECT_SOURCE_DIR}/src/external/jsoncpp/include"|' \
        /tmp/openxr-sdk/src/loader/CMakeLists.txt
    cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
        -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF \
        -DCMAKE_MAP_IMPORTED_CONFIG_RELEASE="Release;None;"
    cmake --build /tmp/openxr-sdk/build
    cmake --install /tmp/openxr-sdk/build
else
    echo "==> OpenXR loader cached at $OPENXR_DIR"
fi

# --- 1. cmake build -------------------------------------------------------
# FFmpeg resolves via pkg-config (brew ffmpeg); Vulkan via VULKAN_SDK / brew.
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR;$(brew --prefix)"
cmake --build build

BIN="$REPO_ROOT/build/mediaplayer_handle_vk_macos"
[ -x "$BIN" ] || { echo "Error: expected binary not found at $BIN" >&2; exit 1; }

if [ "$BUILD_INSTALLER" != "true" ]; then
    echo ""
    echo "Built: $BIN"
    exit 0
fi

# --- 2. .pkg ---------------------------------------------------------------
VERSION="${DISPLAYXR_VERSION:-0.0.0-dev}"
PKG_DIR="$REPO_ROOT/_package"
mkdir -p "$PKG_DIR"
PKG_PATH="$PKG_DIR/DisplayXRMediaPlayer-${VERSION}.pkg"

DISPLAYXR_VERSION="$VERSION" OPENXR_DIR="$OPENXR_DIR" \
    bash "$REPO_ROOT/installer/macos/build_installer.sh" "$BIN" "$PKG_PATH"

echo ""
echo "==> Installer: $PKG_PATH"
ls -lh "$PKG_PATH"
