#!/bin/bash
# Create a macOS .app bundle for the Stereo Media Player demo.
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
#   DISPLAYXR_VERSION   version baked into Info.plist (default 0.0.0-dev)
#   OPENXR_DIR          prefix of the source-built OpenXR loader (for dylibbundler
#                       search path); default /tmp/openxr-install
set -euo pipefail

BIN_SRC="${1:?Usage: $0 <binary-path> [output.app]}"
APP_BUNDLE="${2:-Stereo Media Player.app}"
BINARY_NAME="mediaplayer_handle_vk_macos"
VERSION="${DISPLAYXR_VERSION:-0.0.0-dev}"
OPENXR_DIR="${OPENXR_DIR:-/tmp/openxr-install}"

BUNDLE_DISPLAY_NAME="Stereo Media Player"
BUNDLE_ID="com.displayxr.mediaplayer"

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
</dict>
</plist>
EOF

# --- Shell launcher (CFBundleExecutable) ---
# Sets DYLD/VK env so the bundled loader + MoltenVK ICD are found; does NOT set
# XR_RUNTIME_JSON so the bundled OpenXR loader discovers the system runtime via
# /etc/xdg/openxr/1/active_runtime.json. With no CLI arg, auto-loads the bundled
# sample stereo image (matching the app's "no arg → test pattern" otherwise).
cat > "$APP_BUNDLE/Contents/MacOS/$BUNDLE_DISPLAY_NAME" <<'LAUNCHER'
#!/bin/bash
DIR="$(cd "$(dirname "$0")/../Resources" && pwd)"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="$DIR/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/MoltenVK_icd.json"
cd "$DIR"
if [ "$#" -eq 0 ] && [ -f "$DIR/test_LR_2x1.png" ]; then
    exec "$DIR/mediaplayer_handle_vk_macos" "$DIR/test_LR_2x1.png"
fi
exec "$DIR/mediaplayer_handle_vk_macos" "$@"
LAUNCHER
chmod +x "$APP_BUNDLE/Contents/MacOS/$BUNDLE_DISPLAY_NAME"

# --- Resources: binary + bundled sample image ---
cp "$BIN_SRC" "$APP_BUNDLE/Contents/Resources/$BINARY_NAME"
chmod u+w "$APP_BUNDLE/Contents/Resources/$BINARY_NAME"
if [ -f "$REPO_ROOT/assets/test_LR_2x1.png" ]; then
    cp "$REPO_ROOT/assets/test_LR_2x1.png" "$APP_BUNDLE/Contents/Resources/"
fi

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

declare -A BUNDLED  # basename -> 1, to copy/recurse each lib at most once

# Copy every non-system dependency of $1 into LIBDIR, rewrite the reference to
# @rpath/<base>, and recurse into each newly-copied lib.
bundle_deps() {
    local target="$1" target_dir ref base real
    target_dir="$(cd "$(dirname "$target")" && pwd)"
    while IFS= read -r ref; do
        [ -z "$ref" ] && continue
        case "$ref" in /usr/lib/*|/System/*) continue ;; esac
        base="$(basename "$ref")"
        real="$(resolve_ref "$ref" "$target_dir")"
        if [ -z "$real" ] || [ ! -f "$real" ]; then
            # Unresolvable (e.g. the target's own @rpath id) — just point at our copy
            # if we have it; otherwise leave it (system fallback) and move on.
            [ -f "$LIBDIR/$base" ] && install_name_tool -change "$ref" "@rpath/$base" "$target" 2>/dev/null || true
            continue
        fi
        if [ -z "${BUNDLED[$base]:-}" ]; then
            BUNDLED[$base]=1
            cp -f "$real" "$LIBDIR/$base"
            chmod u+w "$LIBDIR/$base"
            install_name_tool -id "@rpath/$base" "$LIBDIR/$base" 2>/dev/null || true
            bundle_deps "$LIBDIR/$base"
        fi
        install_name_tool -change "$ref" "@rpath/$base" "$target" 2>/dev/null || true
    done < <(otool -L "$target" | tail -n +2 | awk '{print $1}')
}

# MoltenVK is already copied (loaded via ICD, not linked) — register + relink it.
BUNDLED[libMoltenVK.dylib]=1
install_name_tool -id "@rpath/libMoltenVK.dylib" "$LIBDIR/libMoltenVK.dylib" 2>/dev/null || true
bundle_deps "$LIBDIR/libMoltenVK.dylib"

# Walk the binary's dependency closure.
bundle_deps "$BIN"

# Give the binary an rpath rooted at the bundled libs.
install_name_tool -add_rpath "@loader_path/lib" "$BIN" 2>/dev/null || true

# Re-sign everything install_name_tool touched — Apple Silicon SIGKILLs Mach-O
# whose code signature was invalidated by relinking.
for d in "$LIBDIR"/*.dylib; do
    [ -e "$d" ] || continue
    codesign --force --sign - "$d"
done
codesign --force --sign - "$BIN"

echo ".app bundle created: $APP_BUNDLE ($(ls "$LIBDIR" | wc -l | tr -d ' ') bundled dylibs)"
