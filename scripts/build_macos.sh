#!/usr/bin/env bash
#
# scripts/build_macos.sh — Build the demo binary, and optionally the .pkg
# installer for distribution.
#
# Usage:
#   ./scripts/build_macos.sh                run cmake build only
#   ./scripts/build_macos.sh --installer    also build _package/DisplayXRMediaPlayer-*.pkg
#   ./scripts/build_macos.sh --pro          also build Media Player Pro (-DMP_BUILD_PRO=ON);
#                                           with --installer, also _package/DisplayXRMediaPlayerPro-*.pkg
#   ./scripts/build_macos.sh --ocio         OpenColorIO in Pro (brew opencolorio, installed if
#                                           missing; -DMP_OCIO_ROOT=$(brew --prefix opencolorio))
#   ./scripts/build_macos.sh --brew-ffmpeg  LOCAL DEV ONLY: link Homebrew's FFmpeg (pkg-config)
#                                           instead of the slim one (no DASH / SRT). Refused
#                                           with --installer: a .pkg always ships the slim FFmpeg.
#
# Env:
#   DISPLAYXR_VERSION   version string baked into the .pkg + .app Info.plist
#                       (defaults to 0.0.0-dev). CI sets this from the v* git tag.
#   MP_PRO_VERSION      version for the Pro .pkg (defaults to DISPLAYXR_VERSION).
#                       CI sets this from the pro-v* git tag.
#   MP_BUILD_DIR        CMake build directory (default: build).
#   OPENXR_VERSION      OpenXR-SDK release tag for the loader (default 1.1.51).
#   FFMPEG_PREFIX       where scripts/build_ffmpeg_macos.sh installs the slim FFmpeg
#                       (default /tmp/ffmpeg-slim-macos; built once, then cached).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_INSTALLER=false
BUILD_PRO=false
WITH_OCIO=false
BREW_FFMPEG=false
for arg in "${@}"; do
    case "$arg" in
        --installer) BUILD_INSTALLER=true ;;
        --pro) BUILD_PRO=true ;;
        --ocio) WITH_OCIO=true ;;
        --brew-ffmpeg) BREW_FFMPEG=true ;;
        -h|--help) sed -n '1,30p' "$0"; exit 0 ;;
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

# --- 0b. Slim FFmpeg (#201) ------------------------------------------------
# The same pinned, decode-only FFmpeg as Windows / Linux, plus VideoToolbox, Secure
# Transport TLS, DASH (libxml2), SRT (libsrt) and dav1d. Passed as FFMPEG_ROOT, which
# CMake searches exclusively, so a Homebrew FFmpeg on the box is never linked.
CMAKE_EXTRA=()
if [ "$BREW_FFMPEG" = "true" ]; then
    if [ "$BUILD_INSTALLER" = "true" ]; then
        echo "Error: --brew-ffmpeg is for local dev builds; a .pkg ships the slim FFmpeg." >&2
        exit 2
    fi
    echo "==> FFmpeg: Homebrew (pkg-config) - local dev build, no DASH / SRT"
    CMAKE_EXTRA+=(-UFFMPEG_ROOT)
else
    FFMPEG_PREFIX="${FFMPEG_PREFIX:-/tmp/ffmpeg-slim-macos}"
    FFMPEG_PREFIX="$FFMPEG_PREFIX" bash "$REPO_ROOT/scripts/build_ffmpeg_macos.sh"
    CMAKE_EXTRA+=(-DFFMPEG_ROOT="$FFMPEG_PREFIX")
fi

# --- 1. cmake build -------------------------------------------------------
# Vulkan via VULKAN_SDK / brew.
BUILD_DIR="${MP_BUILD_DIR:-build}"
if [ "$BUILD_PRO" = "true" ]; then
    CMAKE_EXTRA+=(-DMP_BUILD_PRO=ON)
fi
if [ "$WITH_OCIO" = "true" ]; then
    [ "$BUILD_PRO" = "true" ] || echo "Note: --ocio only affects Media Player Pro (add --pro)." >&2
    if ! brew list --versions opencolorio >/dev/null 2>&1; then
        echo "==> Installing OpenColorIO (brew install opencolorio)"
        brew install opencolorio
    fi
    CMAKE_EXTRA+=(-DMP_OCIO_ROOT="$(brew --prefix opencolorio)")
fi
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR;$(brew --prefix)" \
    ${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"}
cmake --build "$BUILD_DIR"

BUILD_ABS="$(cd "$BUILD_DIR" && pwd)"
BIN="$BUILD_ABS/mediaplayer_handle_vk_macos"
[ -x "$BIN" ] || { echo "Error: expected binary not found at $BIN" >&2; exit 1; }
PRO_BIN="$BUILD_ABS/mediaplayer_pro_handle_vk_macos"
if [ "$BUILD_PRO" = "true" ]; then
    [ -x "$PRO_BIN" ] || { echo "Error: expected Pro binary not found at $PRO_BIN" >&2; exit 1; }
fi

if [ "$BUILD_INSTALLER" != "true" ]; then
    echo ""
    echo "Built: $BIN"
    [ "$BUILD_PRO" = "true" ] && echo "Built: $PRO_BIN"
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

if [ "$BUILD_PRO" = "true" ]; then
    PRO_VERSION="${MP_PRO_VERSION:-$VERSION}"
    PRO_PKG_PATH="$PKG_DIR/DisplayXRMediaPlayerPro-${PRO_VERSION}.pkg"
    MP_PRODUCT=pro DISPLAYXR_VERSION="$PRO_VERSION" OPENXR_DIR="$OPENXR_DIR" \
        bash "$REPO_ROOT/installer/macos/build_installer.sh" "$PRO_BIN" "$PRO_PKG_PATH"
    echo ""
    echo "==> Pro installer: $PRO_PKG_PATH"
    ls -lh "$PRO_PKG_PATH"
fi
