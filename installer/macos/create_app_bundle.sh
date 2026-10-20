#!/bin/bash
# Create a macOS .app bundle for the Stereo Media Player demo, or (MP_PRODUCT=pro)
# for Media Player Pro (docs/PRO.md) — same bundler, different names/ids/manifest.
#
# Adapted from the model/gauss demo bundles. The media player additionally links
# FFmpeg (libav*/libsw*), whose Homebrew build drags in a deep tree of transitive
# dylibs (x264-free LGPL still pulls lzma, bz2, etc.). We therefore recursively
# walk `otool -L` and relink every non-system dependency into
# Contents/Resources/lib with @rpath. (We deliberately do NOT use dylibbundler:
# it prompts on stdin for any library it can't resolve, which hangs CI forever.)
#
# Not bundled: the DisplayXR runtime dylib — the app defers to the system-installed
# runtime via /etc/xdg/openxr/1/active_runtime.json (registered by the runtime
# .pkg). MoltenVK is loaded via the ICD manifest (not linked), so it's copied +
# pointed at manually; dylibbundler only sees linked libraries.
#
# Usage: ./create_app_bundle.sh <binary-path> [output.app]
# Env:
#   MP_PRODUCT          "base" (default) = Stereo Media Player.app, com.displayxr.mediaplayer,
#                       binary mediaplayer_handle_vk_macos, launcher opens the sample image;
#                       "pro" = Media Player Pro.app, com.displayxr.mediaplayer.pro, binary
#                       mediaplayer_pro_handle_vk_macos, launcher opens nothing (idle screen).
#   DISPLAYXR_VERSION   version baked into Info.plist (default 0.0.0-dev)
#   OPENXR_DIR          prefix of the source-built OpenXR loader (for dylibbundler
#                       search path); default /tmp/openxr-install
set -euo pipefail

MP_PRODUCT="${MP_PRODUCT:-base}"
case "$MP_PRODUCT" in
    base)
        BINARY_NAME="mediaplayer_handle_vk_macos"
        BUNDLE_DISPLAY_NAME="Stereo Media Player"
        BUNDLE_ID="com.displayxr.mediaplayer"
        MANIFEST_BASENAME="mediaplayer_handle_vk_win.displayxr.json"
        LAUNCH_SAMPLE=1 ;;
    pro)
        BINARY_NAME="mediaplayer_pro_handle_vk_macos"
        BUNDLE_DISPLAY_NAME="Media Player Pro"
        BUNDLE_ID="com.displayxr.mediaplayer.pro"
        MANIFEST_BASENAME="mediaplayer_pro_handle_vk_win.displayxr.json"
        LAUNCH_SAMPLE=0 ;;
    *) echo "Error: MP_PRODUCT must be base or pro (got '$MP_PRODUCT')" >&2; exit 2 ;;
esac

BIN_SRC="${1:?Usage: $0 <binary-path> [output.app]}"
APP_BUNDLE="${2:-${BUNDLE_DISPLAY_NAME}.app}"
VERSION="${DISPLAYXR_VERSION:-0.0.0-dev}"
OPENXR_DIR="${OPENXR_DIR:-/tmp/openxr-install}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"

if [ ! -x "$BIN_SRC" ]; then
    echo "Error: binary not found/executable at $BIN_SRC" >&2
    exit 1
fi

echo "Creating .app bundle: $APP_BUNDLE"
rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources/lib"

# --- PkgInfo / Info.plist ---
echo -n "APPL????" > "$APP_BUNDLE/Contents/PkgInfo"
cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>${BUNDLE_DISPLAY_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleName</key>
    <string>${BUNDLE_DISPLAY_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${BUNDLE_DISPLAY_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${VERSION}</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSCameraUsageDescription</key>
    <string>Show a live stereo camera (e.g. a side-by-side webcam or a capture box) on the 3D display.</string>
</dict>
</plist>
EOF

# --- Shell launcher (CFBundleExecutable) ---
# Sets DYLD/VK env so the bundled loader + MoltenVK ICD are found; does NOT set
# XR_RUNTIME_JSON so the bundled OpenXR loader discovers the system runtime via
# /etc/xdg/openxr/1/active_runtime.json. Base player: with no CLI arg, auto-loads
# the bundled sample stereo image (matching the app's "no arg → test pattern"
# otherwise). Pro: no sample — it starts on its idle screen, waiting for an input.
LAUNCHER_PATH="$APP_BUNDLE/Contents/MacOS/$BUNDLE_DISPLAY_NAME"
cat > "$LAUNCHER_PATH" <<'LAUNCHER'
#!/bin/bash
DIR="$(cd "$(dirname "$0")/../Resources" && pwd)"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="$DIR/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/MoltenVK_icd.json"
cd "$DIR"
LAUNCHER
if [ "$LAUNCH_SAMPLE" = "1" ]; then
    cat >> "$LAUNCHER_PATH" <<LAUNCHER
if [ "\$#" -eq 0 ] && [ -f "\$DIR/test_LR_2x1.png" ]; then
    exec "\$DIR/${BINARY_NAME}" "\$DIR/test_LR_2x1.png"
fi
LAUNCHER
fi
cat >> "$LAUNCHER_PATH" <<LAUNCHER
exec "\$DIR/${BINARY_NAME}" "\$@"
LAUNCHER
chmod +x "$APP_BUNDLE/Contents/MacOS/$BUNDLE_DISPLAY_NAME"

# --- Resources: binary + bundled sample image ---
cp "$BIN_SRC" "$APP_BUNDLE/Contents/Resources/$BINARY_NAME"
chmod u+w "$APP_BUNDLE/Contents/Resources/$BINARY_NAME"
if [ "$LAUNCH_SAMPLE" = "1" ] && [ -f "$REPO_ROOT/assets/test_LR_2x1.png" ]; then
    cp "$REPO_ROOT/assets/test_LR_2x1.png" "$APP_BUNDLE/Contents/Resources/"
fi

# --- DisplayXR app manifest + icons (Contents/Resources/displayxr/) ---
# The DisplayXR shell launcher discovers macOS apps by scanning
# /Applications/*.app/Contents/Resources/displayxr/ for <binary>.displayxr.json,
# exactly like the model/gauss/earthview/avatar demo bundles. The manifest basename
# is the real binary's name (the one in Contents/Resources that the launcher execs),
# and it carries no exe_path — the shell derives it. Content is the same manifest the
# Windows installer ships (single source of truth: displayxr/ at the repo root), so
# the app has one id + name on every platform. idle.png/logo.png ride along so the
# player's idle logo resolves at <exe-dir>/displayxr/ instead of the test pattern.
DXR_SRC="$REPO_ROOT/displayxr"
DXR_DST="$APP_BUNDLE/Contents/Resources/displayxr"
DXR_MANIFEST_SRC="$DXR_SRC/$MANIFEST_BASENAME"
mkdir -p "$DXR_DST"
for f in icon.png icon_sbs.png idle.png logo.png; do
    if [ ! -f "$DXR_SRC/$f" ]; then
        echo "Error: $DXR_SRC/$f missing (app manifest icon / idle logo)" >&2
        exit 1
    fi
    cp "$DXR_SRC/$f" "$DXR_DST/"
done
if [ ! -f "$DXR_MANIFEST_SRC" ]; then
    echo "Error: app manifest $DXR_MANIFEST_SRC missing" >&2
    exit 1
fi
cp "$DXR_MANIFEST_SRC" "$DXR_DST/${BINARY_NAME}.displayxr.json"

# --- MoltenVK (loaded via ICD, not linked) + ICD manifest ---
MVK_SRC="$(find "$(brew --prefix molten-vk 2>/dev/null || echo "$BREW_PREFIX")/lib" -maxdepth 1 -name 'libMoltenVK*.dylib' 2>/dev/null | head -1)"
if [ -z "$MVK_SRC" ]; then
    echo "Error: libMoltenVK.dylib not found — \`brew install molten-vk\`" >&2
    exit 1
fi
cp -P "$MVK_SRC" "$APP_BUNDLE/Contents/Resources/lib/libMoltenVK.dylib"
cat > "$APP_BUNDLE/Contents/Resources/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "lib/libMoltenVK.dylib",
        "api_version": "1.2.0",
        "is_portability_driver": true
    }
}
EOF

# --- Recursively gather + relink every non-system dependency into Resources/lib
#     with @rpath (FFmpeg + transitive, Vulkan loader, OpenXR loader, MoltenVK). ---
LIBDIR="$APP_BUNDLE/Contents/Resources/lib"
BIN="$APP_BUNDLE/Contents/Resources/$BINARY_NAME"
SEARCH_DIRS=("$OPENXR_DIR/lib" "$BREW_PREFIX/lib")

# Resolve a Mach-O load-command reference ($1) seen from a consumer dir ($2) to an
# absolute on-disk path, or "" if it can't be found / is our own already-copied lib.
resolve_ref() {
    local ref="$1" consumer_dir="$2" base d
    base="$(basename "$ref")"
    case "$ref" in
        /*)                  echo "$ref" ;;
        @loader_path/*)      echo "${consumer_dir}/${ref#@loader_path/}" ;;
        @executable_path/*)  echo "${consumer_dir}/${ref#@executable_path/}" ;;
        *)  # @rpath/<x> or a bare name — search known dirs, then brew opt kegs.
            for d in "${SEARCH_DIRS[@]}" "$BREW_PREFIX"/opt/*/lib; do
                if [ -f "$d/$base" ]; then echo "$d/$base"; return; fi
            done
            echo "" ;;
    esac
}

# Copy every non-system dependency of $1 into LIBDIR, rewrite the reference to
# @rpath/<base>, and recurse into each newly-copied lib. Presence in LIBDIR is the
# "already processed" guard (macOS ships Bash 3.2 — no associative arrays).
bundle_deps() {
    local target="$1" target_dir ref base real
    target_dir="$(cd "$(dirname "$target")" && pwd)"
    while IFS= read -r ref; do
        [ -z "$ref" ] && continue
        case "$ref" in /usr/lib/*|/System/*) continue ;; esac
        base="$(basename "$ref")"
        real="$(resolve_ref "$ref" "$target_dir")"
        if [ -z "$real" ] || [ ! -f "$real" ]; then
            # Unresolvable (e.g. the target's own @rpath id) — point at our copy if
            # we have one; otherwise leave it (system fallback) and move on.
            if [ -f "$LIBDIR/$base" ]; then
                install_name_tool -change "$ref" "@rpath/$base" "$target" 2>/dev/null || true
            fi
            continue
        fi
        if [ ! -f "$LIBDIR/$base" ]; then
            cp -f "$real" "$LIBDIR/$base"
            chmod u+w "$LIBDIR/$base"
            install_name_tool -id "@rpath/$base" "$LIBDIR/$base" 2>/dev/null || true
            bundle_deps "$LIBDIR/$base"
        fi
        install_name_tool -change "$ref" "@rpath/$base" "$target" 2>/dev/null || true
    done < <(otool -L "$target" | tail -n +2 | awk '{print $1}')
}

# MoltenVK is already copied (loaded via ICD, not linked) — relink it + its deps.
install_name_tool -id "@rpath/libMoltenVK.dylib" "$LIBDIR/libMoltenVK.dylib" 2>/dev/null || true
bundle_deps "$LIBDIR/libMoltenVK.dylib"

# Walk the binary's dependency closure.
bundle_deps "$BIN"

# --- rpaths: bundle-relative ONLY, @loader_path/lib first ---
# The linker leaves the build tree's search dirs in the binary as LC_RPATHs
# (/opt/homebrew/lib, /tmp/openxr-install/lib — needed for dev runs from build/).
# Shipped, they are searched BEFORE @loader_path/lib, so on a Mac with Homebrew
# SDL3 / FFmpeg / vulkan-loader the app loaded THOSE instead of the bundled copies
# (v1.14.2). Strip them from every Mach-O in the bundle; -delete_rpath removes one
# occurrence per call, so loop until none is left.
list_rpaths() {
    otool -l "$1" | awk '/cmd LC_RPATH/{r=1} r && $1=="path"{print $2; r=0}'
}
strip_rpaths() {   # $1 = Mach-O, $2 = "all" | "absolute"
    local target="$1" mode="$2" rp found
    while :; do
        found=""
        while IFS= read -r rp; do
            [ -z "$rp" ] && continue
            case "$rp" in
                @loader_path|@loader_path/*|@executable_path|@executable_path/*)
                    [ "$mode" = "all" ] || continue ;;
            esac
            found="$rp"; break
        done < <(list_rpaths "$target")
        [ -z "$found" ] && break
        install_name_tool -delete_rpath "$found" "$target"
    done
}
# The binary: drop every rpath, then add the one it needs — the bundled libs.
strip_rpaths "$BIN" all
install_name_tool -add_rpath "@loader_path/lib" "$BIN"
# Bundled dylibs: keep their own bundle-relative rpaths (e.g. x265's
# @loader_path/../lib), drop absolute ones.
for d in "$LIBDIR"/*.dylib; do
    [ -e "$d" ] || continue
    strip_rpaths "$d" absolute
done

# Re-sign everything install_name_tool touched — Apple Silicon SIGKILLs Mach-O
# whose code signature was invalidated by relinking. Must stay AFTER the last
# install_name_tool edit.
for d in "$LIBDIR"/*.dylib; do
    [ -e "$d" ] || continue
    codesign --force --sign - "$d"
done
codesign --force --sign - "$BIN"

# Guard: no Mach-O in the .app may reach outside it (rpath, dependency, install id)
# or carry a broken signature. Fails the build instead of shipping v1.14.2 again.
bash "$REPO_ROOT/installer/macos/check_bundle_paths.sh" "$APP_BUNDLE"

echo ".app bundle created: $APP_BUNDLE ($(ls "$LIBDIR" | wc -l | tr -d ' ') bundled dylibs)"
