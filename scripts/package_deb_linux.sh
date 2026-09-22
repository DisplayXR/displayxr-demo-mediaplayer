#!/usr/bin/env bash
# Package the Linux demo as a Debian package (.deb) — runtime #781 Phase 3
# (demo installers). Companion to build_linux.sh: builds the demo (if needed)
# then wraps the binary + its OpenXR loader + assets into an installable .deb.
#
#   ./scripts/package_deb_linux.sh            # build (if needed) + package
#   ./scripts/package_deb_linux.sh --no-build # package an existing build-deb/
#
# Output: dist/<pkg>_<ver>_<arch>.deb
#
# Payload (installed layout):
#   /usr/lib/displayxr-demos/<app>/<binary>          the demo executable (RUNPATH=$ORIGIN)
#   /usr/lib/displayxr-demos/<app>/libopenxr_loader.so*  bundled OpenXR loader
#   /usr/lib/displayxr-demos/<app>/lib{avcodec,avformat,avutil,swscale,swresample}.so*
#                                                     bundled private slim FFmpeg
#   /usr/lib/displayxr-demos/<app>/assets/...         bundled sample assets
#   /usr/bin/<pkg>                                    launcher wrapper (on PATH)
#   /usr/share/applications/<pkg>.desktop            menu entry
#
# The demo is an OpenXR app → Depends: displayxr-runtime. With the runtime .deb
# installed, the loader resolves the runtime via /etc/xdg/openxr/1/active_runtime.json
# automatically — no env vars. The wrapper sets OXR_ENABLE_VK_NATIVE_COMPOSITOR=1
# (Linux vk_native path).
#
# ONE .deb FOR 22.04, 24.04 AND 26.04 (#76). The package must not depend on
# anything whose package name or soname differs between those releases, because
# the Linux meta-bundle ships exactly one displayxr-mediaplayer_*_amd64.deb:
#   * SDL3 is linked STATICALLY (CMakeLists.txt) — Ubuntu 24.04 has no libsdl3
#     package at all. SDL still dlopen()s its X11/Wayland/audio backends.
#   * FFmpeg is a PRIVATE slim build (scripts/build_ffmpeg_linux.sh) bundled
#     next to the binary — distro sonames are libavcodec58 / 60 / 62 on
#     22.04 / 24.04 / 26.04.
#   * The release .deb is built on the OLDEST supported release (22.04, CI Deb
#     job), which sets the glibc / libstdc++ floor.
# Depends is derived by dpkg-shlibdeps from the binary + bundled libs, and every
# DT_NEEDED soname must be either bundled or on STABLE_SONAMES below — a new
# system library fails the build instead of silently narrowing the releases the
# package installs on. CI then apt-installs the .deb into clean 22.04 / 24.04 /
# 26.04 containers and fails on any unresolvable Depends or `ldd -r` error.
#
# --- Per-demo config (the ONLY part that differs between demos) -------------
APP="mediaplayer"                                   # short id (dir + component)
PKG="displayxr-mediaplayer"                         # .deb package + wrapper name
DISPLAY_NAME="DisplayXR Stereo Media Player"
DESCRIPTION="Stereo 3D photo & video player (SBS / LR / anaglyph) for glasses-free 3D displays."
BINARY="mediaplayer_handle_vk_linux"                # build/<BINARY>
DESKTOP_CATEGORIES="AudioVideo;Player;"
ASSETS_SUBDIR=""                                    # defaults-only: no misc assets/ dir
# Staged flat next to the binary (Windows-installer parity): one sample stereo
# clip the user can Open. The idle screen comes from the displayxr/ sidecar below.
DEFAULT_ASSETS=("assets/test_LR_2x1.png:test_LR_2x1.png")

# System sonames the binary / bundled libs may link (DT_NEEDED). Each one's
# package exists under the SAME name on Ubuntu 22.04, 24.04 and 26.04. Adding a
# soname here is a claim about all three releases — CI's install matrix checks it.
STABLE_SONAMES=(
  libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux-x86-64.so.2
  libstdc++.so.6 libgcc_s.so.1
  libvulkan.so.1
  libva.so.2 libva-drm.so.2      # FFmpeg VAAPI hwaccel (libva2 / libva-drm2)
  libz.so.1                      # FFmpeg zlib (zlib1g)
)
# Loaded at RUNTIME by the static SDL3 (dlopen, so invisible to dpkg-shlibdeps).
# X11 is required: the player binds its window via XR_DXR_xlib_window_binding.
EXTRA_DEPENDS="libx11-6, libxext6"
EXTRA_RECOMMENDS="libpulse0, libxcursor1, libxi6, libxrandr2, libxfixes3"
# VAAPI drivers are hardware-specific; without one FFmpeg falls back to software.
EXTRA_SUGGESTS="va-driver-all"

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-deb}"   # separate from the dev build/ (system FFmpeg)
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
FFMPEG_PREFIX="${FFMPEG_PREFIX:-/tmp/ffmpeg-slim-linux}"

NO_BUILD=0
for a in "$@"; do case "$a" in
  --no-build) NO_BUILD=1 ;;
  *) echo "Unknown option: $a (supported: --no-build)" >&2; exit 2 ;;
esac; done

command -v dpkg-deb >/dev/null 2>&1 || { echo "error: dpkg-deb not found — Debian/Ubuntu host/container only." >&2; exit 1; }

command -v dpkg-shlibdeps >/dev/null 2>&1 || { echo "error: dpkg-shlibdeps not found — install dpkg-dev." >&2; exit 1; }

# mediaplayer's CMake emits the binary directly under the build dir (no linux/
# subdir — that's a modelviewer layout quirk), matching build_linux.sh. The
# portable build also stages the bundled libs (FFmpeg + OpenXR loader) there.
BIN="$BUILD_DIR/$BINARY"
if [ "$NO_BUILD" = 0 ]; then
  echo "==> Building the private slim FFmpeg (scripts/build_ffmpeg_linux.sh)"
  FFMPEG_PREFIX="$FFMPEG_PREFIX" "$ROOT/scripts/build_ffmpeg_linux.sh"
  echo "==> Building via scripts/build_linux.sh (portable: static SDL3, bundled FFmpeg)"
  BUILD_DIR="$BUILD_DIR" FFMPEG_ROOT="$FFMPEG_PREFIX" MP_LINUX_PORTABLE=ON "$ROOT/scripts/build_linux.sh"
fi
[ -x "$BIN" ] || { echo "error: demo binary $BIN missing (build, or drop --no-build)." >&2; exit 1; }
# The static SDL3 must have compiled its X11 video + audio backends as runtime
# dlopen()s. A backend whose dev headers were missing on the build host is
# silently dropped (no window / no sound on the user's box), and one linked
# directly would add a DT_NEEDED — both are packaging bugs, so check here.
SDL_CFG="$(find "$BUILD_DIR/_deps/sdl3-build" -name SDL_build_config.h 2>/dev/null | head -1)"
[ -n "$SDL_CFG" ] || { echo "error: SDL_build_config.h not found — is SDL3 built from source in $BUILD_DIR?" >&2; exit 1; }
for want in SDL_VIDEO_DRIVER_X11_DYNAMIC SDL_VIDEO_DRIVER_X11_DYNAMIC_XEXT \
            SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC SDL_AUDIO_DRIVER_PIPEWIRE_DYNAMIC SDL_AUDIO_DRIVER_ALSA_DYNAMIC; do
  grep -q "^#define $want \"" "$SDL_CFG" || { echo "error: static SDL3 built without $want (missing -dev headers?)." >&2; exit 1; }
done
grep -q '^MP_LINUX_PORTABLE:BOOL=ON' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null \
  || { echo "error: $BUILD_DIR is not an MP_LINUX_PORTABLE build — it would link the distro FFmpeg." >&2; exit 1; }

# --- Version: git describe → Debian-legal upstream version (v* tags only) ---
RAW="$(git -C "$ROOT" describe --tags --always --dirty --match 'v[0-9]*' 2>/dev/null || echo 0.0.0)"
VERSION="$(echo "$RAW" | sed -e 's/^v//' -e 's/-dirty$/+dirty/' -e 's/-\([0-9]\+\)-g/+\1.g/')"
case "$VERSION" in [0-9]*) : ;; *) VERSION="0.0.0+g$VERSION" ;; esac
ARCH="$(dpkg --print-architecture)"

STAGE="$DIST_DIR/${PKG}_${VERSION}_${ARCH}"
APPDIR="$STAGE/usr/lib/displayxr-demos/$APP"
echo "==> Staging $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$APPDIR" "$STAGE/usr/bin" "$STAGE/usr/share/applications"

install -m 0755 "$BIN" "$APPDIR/$BINARY"

# Bundle the libs the portable build staged next to the binary: the private
# FFmpeg and the OpenXR loader (built from source by build_linux.sh, so the .deb
# doesn't depend on a distro loader whose version may differ from the demo's
# pin). -P keeps the soname symlinks. The binary finds them via RUNPATH=$ORIGIN.
for f in "$BUILD_DIR"/lib*.so*; do
  [ -e "$f" ] && cp -P "$f" "$APPDIR/"
done
for so in libopenxr_loader.so.1 libavcodec.so libavformat.so libavutil.so libswscale.so libswresample.so; do
  ls "$APPDIR/$so"* >/dev/null 2>&1 || { echo "error: bundled $so* missing from $BUILD_DIR." >&2; exit 1; }
done
objdump -p "$APPDIR/$BINARY" | grep -q 'RUNPATH *\$ORIGIN$' \
  || { echo "error: $BINARY lacks RUNPATH \$ORIGIN (build-tree paths would ship)." >&2; objdump -p "$APPDIR/$BINARY" | grep -E 'R(UN)?PATH' >&2; exit 1; }

# Bundle assets (sample stereo media etc.).
if [ -n "$ASSETS_SUBDIR" ] && [ -d "$ROOT/$ASSETS_SUBDIR" ]; then
  cp -aR "$ROOT/$ASSETS_SUBDIR" "$APPDIR/assets"
fi

# Default assets, staged FLAT next to the binary (the app resolves them via
# /proc/self/exe — the Windows-installer layout; see the modelviewer template).
for pair in "${DEFAULT_ASSETS[@]:-}"; do
  [ -n "$pair" ] || continue
  src="$ROOT/${pair%%:*}"; dst="${pair##*:}"
  if [ -f "$src" ]; then
    install -m 0644 "$src" "$APPDIR/$dst"
    echo "==> default asset staged: $dst ($(du -h "$src" | cut -f1))"
  else
    echo "warn: default asset '${pair%%:*}' not found." >&2
  fi
done

# The displayxr/ sidecar next to the binary: LoadIdleLogo resolves
# <exe-dir>/displayxr/{idle,logo}.png for the no-media idle screen, and the
# workspace manifest/icons ride along (manifest renamed to the Linux binary).
mkdir -p "$APPDIR/displayxr"
for f in idle.png logo.png icon.png icon_sbs.png; do
  [ -f "$ROOT/displayxr/$f" ] && install -m 0644 "$ROOT/displayxr/$f" "$APPDIR/displayxr/$f"
done
if [ -f "$ROOT/displayxr/mediaplayer_handle_vk_win.displayxr.json" ]; then
  install -m 0644 "$ROOT/displayxr/mediaplayer_handle_vk_win.displayxr.json" \
    "$APPDIR/displayxr/$BINARY.displayxr.json"
fi
echo "==> displayxr/ sidecar staged (idle screen + manifest)"

# Launcher wrapper on PATH.
cat > "$STAGE/usr/bin/$PKG" <<EOF
#!/bin/sh
# DisplayXR demo launcher. The runtime .deb registers the OpenXR ActiveRuntime,
# so no env vars are needed; the binary finds its bundled libs via RUNPATH, so
# we only select the Linux vk_native compositor.
DIR="/usr/lib/displayxr-demos/$APP"
export OXR_ENABLE_VK_NATIVE_COMPOSITOR="\${OXR_ENABLE_VK_NATIVE_COMPOSITOR:-1}"
exec "\$DIR/$BINARY" "\$@"
EOF
chmod 0755 "$STAGE/usr/bin/$PKG"

# Desktop menu entry.
cat > "$STAGE/usr/share/applications/$PKG.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$DISPLAY_NAME
Comment=$DESCRIPTION
Exec=$PKG
Terminal=false
Categories=$DESKTOP_CATEGORIES
EOF

# --- Depends ------------------------------------------------------------------
# 1. Every DT_NEEDED of the binary + bundled libs must be bundled or on
#    STABLE_SONAMES (see the header): fail loudly on anything else.
ELF_FILES=("$APPDIR/$BINARY")
while IFS= read -r f; do ELF_FILES+=("$f"); done < <(find "$APPDIR" -maxdepth 1 -type f -name 'lib*.so*' | sort)
bad=""
for so in $(objdump -p "${ELF_FILES[@]}" 2>/dev/null | awk '/NEEDED/{print $2}' | sort -u); do
  [ -e "$APPDIR/$so" ] && continue
  ok=0; for s in "${STABLE_SONAMES[@]}"; do [ "$so" = "$s" ] && ok=1 && break; done
  [ "$ok" = 1 ] || bad="$bad $so"
done
if [ -n "$bad" ]; then
  echo "error: DT_NEEDED on system libraries not known to share a package name across" >&2
  echo "       Ubuntu 22.04/24.04/26.04:$bad" >&2
  echo "       Bundle/statically link them, or add to STABLE_SONAMES once CI's install matrix proves them." >&2
  exit 1
fi
# 2. dpkg-shlibdeps maps those sonames to (versioned) packages. The bundled libs
#    are private (-l$APPDIR, no shlibs info) and are skipped.
SHLIBS_TMP="$(mktemp -d)"
mkdir -p "$SHLIBS_TMP/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" > "$SHLIBS_TMP/debian/control"
LIB_DEPENDS="$(cd "$SHLIBS_TMP" && dpkg-shlibdeps -O --ignore-missing-info -l"$APPDIR" "${ELF_FILES[@]}" \
               | sed -n 's/^shlibs:Depends=//p')"
rm -rf "$SHLIBS_TMP"
[ -n "$LIB_DEPENDS" ] || { echo "error: dpkg-shlibdeps produced no Depends." >&2; exit 1; }
DEPENDS="displayxr-runtime, $LIB_DEPENDS, $EXTRA_DEPENDS"
echo "==> Depends: $DEPENDS"
echo "==> Recommends: $EXTRA_RECOMMENDS"
echo "==> glibc floor: $(objdump -T "${ELF_FILES[@]}" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
INSTALLED_KB="$(du -sk "$STAGE/usr" | cut -f1)"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: graphics
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Recommends: $EXTRA_RECOMMENDS
Suggests: $EXTRA_SUGGESTS
Installed-Size: $INSTALLED_KB
Maintainer: The DisplayXR Project <noreply@displayxr.dev>
Homepage: https://github.com/DisplayXR/displayxr-demo-$APP
Description: $DISPLAY_NAME
 $DESCRIPTION
 .
 A DisplayXR demo OpenXR app for glasses-free 3D displays. Requires the
 DisplayXR runtime (Depends: displayxr-runtime); runs on whichever display
 processor is active (sim-display fallback, or the Leia SR plug-in when
 installed). Launch from the menu or run '$PKG'.
EOF

mkdir -p "$DIST_DIR"
DEB="$DIST_DIR/${PKG}_${VERSION}_${ARCH}.deb"
if command -v fakeroot >/dev/null 2>&1; then
  fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
else
  dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
fi

echo ""
echo "==> $DEB"
dpkg-deb --info "$DEB" | sed 's/^/    /'
dpkg-deb --contents "$DEB" | sed 's/^/    /'
