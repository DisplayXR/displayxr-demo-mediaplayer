#!/usr/bin/env bash
# Build the macOS .pkg for the Stereo Media Player demo.
#
# Usage:
#   DISPLAYXR_VERSION=1.4.0 ./build_installer.sh <binary-path> [output.pkg]
#
# Two-stage build (mirroring the model/gauss demos + displayxr-runtime):
#   1. create_app_bundle.sh  → Applications/Stereo Media Player.app  (bundles
#      the binary + FFmpeg/Vulkan/OpenXR dylibs + MoltenVK ICD via dylibbundler)
#   2. pkgbuild --root <staging>          → mediaplayer.pkg  (component .pkg)
#   3. productbuild --distribution ...    → DisplayXRMediaPlayer-<v>.pkg
#
# Ad-hoc signed only (Signature=adhoc). Developer ID signing is a parallel effort.
# Runtime dependency: expects the DisplayXR runtime .pkg installed first (its
# postinstall registers /etc/xdg/openxr/1/active_runtime.json).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_SRC="${1:?Usage: $0 <binary-path> [output.pkg]}"
VERSION="${DISPLAYXR_VERSION:-0.0.0-dev}"
OUTPUT_PKG="${2:-$(pwd)/DisplayXRMediaPlayer-${VERSION}.pkg}"

BIN_SRC="$(cd "$(dirname "$BIN_SRC")" && pwd)/$(basename "$BIN_SRC")"
OUTPUT_PKG_DIR="$(cd "$(dirname "$OUTPUT_PKG")" && pwd)"
OUTPUT_PKG="$OUTPUT_PKG_DIR/$(basename "$OUTPUT_PKG")"
mkdir -p "$OUTPUT_PKG_DIR"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "==> Building DisplayXRMediaPlayer installer"
echo "    version:  $VERSION"
echo "    binary:   $BIN_SRC"
echo "    output:   $OUTPUT_PKG"

# --- Stage payload: .app under Applications/ ---
PAYLOAD_ROOT="$WORK_DIR/payload"
APPS_DIR="$PAYLOAD_ROOT/Applications"
mkdir -p "$APPS_DIR"
( cd "$APPS_DIR" && DISPLAYXR_VERSION="$VERSION" \
    bash "$SCRIPT_DIR/create_app_bundle.sh" "$BIN_SRC" "Stereo Media Player.app" )

# Strip stray AppleDouble / .DS_Store metadata.
find "$PAYLOAD_ROOT" \( -name '._*' -o -name '.DS_Store' \) -delete

# --- pkgbuild: component package ---
COMPONENT_PKG="$WORK_DIR/mediaplayer.pkg"
pkgbuild \
    --root "$PAYLOAD_ROOT" \
    --identifier com.displayxr.mediaplayer \
    --version "$VERSION" \
    --install-location / \
    "$COMPONENT_PKG"

# --- productbuild: distribution wrapper ---
DIST_XML="$SCRIPT_DIR/Distribution.xml"
RESOURCES_DIR="$SCRIPT_DIR/resources"
[ -f "$DIST_XML" ]    || { echo "Error: $DIST_XML not found" >&2; exit 1; }
[ -d "$RESOURCES_DIR" ] || { echo "Error: $RESOURCES_DIR not found" >&2; exit 1; }

productbuild \
    --distribution "$DIST_XML" \
    --resources "$RESOURCES_DIR" \
    --package-path "$WORK_DIR" \
    "$OUTPUT_PKG"

echo "==> Built: $OUTPUT_PKG"
ls -lh "$OUTPUT_PKG"
