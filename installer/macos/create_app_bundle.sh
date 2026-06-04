#!/bin/bash
# Create a macOS .app bundle for the Stereo Media Player demo.
#
# Adapted from the model/gauss demo bundles. The media player additionally links
# FFmpeg (libav*/libsw*), whose Homebrew build drags in a deep tree of transitive
# dylibs (x264-free LGPL still pulls lzma, bz2, etc.). We therefore use
# `dylibbundler` to recursively gather + relink every non-system dependency into
# Contents/Resources/lib with @rpath, rather than hand-copying a fixed list.
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
command -v dylibbundler >/dev/null 2>&1 || { echo "Error: dylibbundler not found — \`brew install dylibbundler\`" >&2; exit 1; }

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

# --- Gather + relink every linked non-system dependency (FFmpeg + transitive,
#     Vulkan loader, OpenXR loader) into Resources/lib with @rpath. ---
BIN="$APP_BUNDLE/Contents/Resources/$BINARY_NAME"
dylibbundler --overwrite-files --bundle-deps \
    --fix-file "$BIN" \
    --dest-dir "$APP_BUNDLE/Contents/Resources/lib" \
    --install-path "@rpath/" \
    --search-path "$OPENXR_DIR/lib" \
    --search-path "$BREW_PREFIX/lib"

# dylibbundler relinks against @rpath/<name>; ensure the binary has an rpath that
# resolves it (it copies libs next to itself under Resources/lib).
install_name_tool -add_rpath "@loader_path/lib" "$BIN" 2>/dev/null || true

# --- Re-sign everything dylibbundler/install_name_tool touched ---
# Apple Silicon SIGKILLs Mach-O whose signature was invalidated by relinking.
for d in "$APP_BUNDLE/Contents/Resources/lib"/*.dylib; do
    [ -e "$d" ] || continue
    codesign --force --sign - "$d"
done
codesign --force --sign - "$BIN"

echo ".app bundle created: $APP_BUNDLE"
