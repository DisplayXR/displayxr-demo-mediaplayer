#!/usr/bin/env bash
# Build the macOS .pkg for the Stereo Media Player demo.
#
# Usage:
#   DISPLAYXR_VERSION=1.4.0 ./build_installer.sh <binary-path> [output.pkg]
#   MP_PRODUCT=pro DISPLAYXR_VERSION=0.1.0 ./build_installer.sh <pro-binary-path> [output.pkg]
#
# MP_PRODUCT=pro builds Media Player Pro (docs/PRO.md) instead: "Media Player Pro.app",
# component identifier com.displayxr.mediaplayer.pro (so it installs ALONGSIDE the
# Stereo Media Player, never over it), installer/macos/pro/Distribution.xml +
# welcome.html, default output DisplayXRMediaPlayerPro-<v>.pkg.
#
# Two-stage build (mirroring the model/gauss demos + displayxr-runtime):
#   1. create_app_bundle.sh  → Applications/Stereo Media Player.app  (bundles
#      the binary + FFmpeg/Vulkan/OpenXR dylibs + MoltenVK ICD via dylibbundler)
#   2. pkgbuild --root <staging>          → mediaplayer.pkg  (component .pkg)
#   3. productbuild --distribution ...    → DisplayXRMediaPlayer-<v>.pkg
#
# Signing: the payload is signed by scripts/macos_sign_notarize.sh sign-tree as the
# last step before pkgbuild — Developer ID + hardened runtime when
# DXR_MACOS_APP_IDENTITY is set (CI on release tags), ad-hoc otherwise. The product
# archive is signed + notarized afterwards by CI (build-macos.yml), not here.
# Runtime dependency: expects the DisplayXR runtime .pkg installed first (its
# postinstall registers /etc/xdg/openxr/1/active_runtime.json).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_SRC="${1:?Usage: $0 <binary-path> [output.pkg]}"
VERSION="${DISPLAYXR_VERSION:-0.0.0-dev}"
MP_PRODUCT="${MP_PRODUCT:-base}"
case "$MP_PRODUCT" in
    base)
        PRODUCT_LABEL="DisplayXRMediaPlayer"
        APP_NAME="Stereo Media Player.app"
        PKG_ID="com.displayxr.mediaplayer"
        COMPONENT_NAME="mediaplayer.pkg"
        DIST_XML="$SCRIPT_DIR/Distribution.xml"
        WELCOME="" ;;   # resources/ as-is
    pro)
        PRODUCT_LABEL="DisplayXRMediaPlayerPro"
        APP_NAME="Media Player Pro.app"
        PKG_ID="com.displayxr.mediaplayer.pro"
        COMPONENT_NAME="mediaplayer_pro.pkg"
        DIST_XML="$SCRIPT_DIR/pro/Distribution.xml"
        WELCOME="$SCRIPT_DIR/pro/welcome.html" ;;
    *) echo "Error: MP_PRODUCT must be base or pro (got '$MP_PRODUCT')" >&2; exit 2 ;;
esac
OUTPUT_PKG="${2:-$(pwd)/${PRODUCT_LABEL}-${VERSION}.pkg}"

BIN_SRC="$(cd "$(dirname "$BIN_SRC")" && pwd)/$(basename "$BIN_SRC")"
OUTPUT_PKG_DIR="$(cd "$(dirname "$OUTPUT_PKG")" && pwd)"
OUTPUT_PKG="$OUTPUT_PKG_DIR/$(basename "$OUTPUT_PKG")"
mkdir -p "$OUTPUT_PKG_DIR"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "==> Building ${PRODUCT_LABEL} installer"
echo "    version:  $VERSION"
echo "    binary:   $BIN_SRC"
echo "    output:   $OUTPUT_PKG"

# --- Stage payload: .app under Applications/ ---
PAYLOAD_ROOT="$WORK_DIR/payload"
APPS_DIR="$PAYLOAD_ROOT/Applications"
mkdir -p "$APPS_DIR"
( cd "$APPS_DIR" && DISPLAYXR_VERSION="$VERSION" MP_PRODUCT="$MP_PRODUCT" \
    bash "$SCRIPT_DIR/create_app_bundle.sh" "$BIN_SRC" "$APP_NAME" )

# Strip stray AppleDouble / .DS_Store metadata.
find "$PAYLOAD_ROOT" \( -name '._*' -o -name '.DS_Store' \) -delete

# --- Final code signature on the payload ---
# MUST stay the last thing that touches the payload: any edit to a Mach-O or to
# the bundle after this breaks the seal. Entitlements come from
# installer/macos/entitlements/map. With DXR_MACOS_SIGN_FALLBACK=adhoc (CI on
# signed tag builds) a failed Developer ID signature (typically Apple's
# timestamp server) degrades to ad-hoc instead of failing the build, and is
# recorded in $DXR_MACOS_SIGN_STATUS_FILE so CI does not try to notarize it.
SIGN="$SCRIPT_DIR/../../scripts/macos_sign_notarize.sh"
ENT_MAP="$SCRIPT_DIR/entitlements/map"
if ! "$SIGN" sign-tree "$PAYLOAD_ROOT" --entitlements-map "$ENT_MAP"; then
    if [ -n "${DXR_MACOS_APP_IDENTITY:-}" ] && [ "${DXR_MACOS_SIGN_FALLBACK:-}" = "adhoc" ]; then
        echo "::warning::Developer ID signing of $APP_NAME failed - falling back to ad-hoc; this .pkg will NOT be notarized"
        [ -n "${DXR_MACOS_SIGN_STATUS_FILE:-}" ] && echo "adhoc-fallback $PRODUCT_LABEL" >> "$DXR_MACOS_SIGN_STATUS_FILE"
        DXR_MACOS_APP_IDENTITY="" "$SIGN" sign-tree "$PAYLOAD_ROOT" --entitlements-map "$ENT_MAP"
    else
        exit 1
    fi
fi

# --- pkgbuild: component package ---
COMPONENT_PKG="$WORK_DIR/$COMPONENT_NAME"
pkgbuild \
    --root "$PAYLOAD_ROOT" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
    --install-location / \
    "$COMPONENT_PKG"

# --- productbuild: distribution wrapper ---
RESOURCES_DIR="$SCRIPT_DIR/resources"
[ -f "$DIST_XML" ]    || { echo "Error: $DIST_XML not found" >&2; exit 1; }
[ -d "$RESOURCES_DIR" ] || { echo "Error: $RESOURCES_DIR not found" >&2; exit 1; }
if [ -n "$WELCOME" ]; then
    # Pro: the shared LICENSE + Pro's own welcome page (productbuild copies the whole
    # --resources dir, so a separate dir keeps the base .pkg's resources unchanged).
    [ -f "$WELCOME" ] || { echo "Error: $WELCOME not found" >&2; exit 1; }
    mkdir -p "$WORK_DIR/resources"
    cp "$RESOURCES_DIR/LICENSE" "$WORK_DIR/resources/LICENSE"
    cp "$WELCOME" "$WORK_DIR/resources/welcome.html"
    RESOURCES_DIR="$WORK_DIR/resources"
fi

productbuild \
    --distribution "$DIST_XML" \
    --resources "$RESOURCES_DIR" \
    --package-path "$WORK_DIR" \
    "$OUTPUT_PKG"

echo "==> Built: $OUTPUT_PKG"
ls -lh "$OUTPUT_PKG"
