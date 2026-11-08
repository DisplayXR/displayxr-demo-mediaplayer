#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the PRIVATE, slim, DECODE-ONLY FFmpeg that the macOS .app bundles (#201).
#
# Why not Homebrew's FFmpeg: it is a GPL build with every external library
# (x264/x265/svt-av1/vpx/vmaf/openssl/...), so the .app carried ~40 transitive
# Homebrew dylibs, its features were whatever the build box's Homebrew happened to
# be, and it had neither DASH (no libxml2) nor SRT (no libsrt). macOS now ships the
# same slim FFmpeg as Windows (scripts/build-ffmpeg-slim.sh) and Linux
# (scripts/build_ffmpeg_linux.sh): same FFmpeg commit, same component lists.
# Shared libs (LGPL relinking story as on the other platforms); create_app_bundle.sh
# copies them into Contents/Resources/lib and relinks them to @rpath.
#
# macOS-only differences from the Linux build:
#   * VideoToolbox hwaccel (H.264, HEVC incl. 10-bit, ProRes, VP9, AV1 on chips
#     that decode it, MPEG-1/2/4, H.263). System frameworks: no dylib to ship.
#   * TLS through Secure Transport (Security.framework), the macOS counterpart of
#     the Windows build's SChannel: https://, rtmps://, rtsps://, HLS / DASH over
#     https, with no OpenSSL to bundle.
#   * libdav1d, static: FFmpeg's native AV1 decoder is hardware-only, and Apple
#     Silicon before M3 has no AV1 decode, so without dav1d AV1 would not play at
#     all on M1 / M2 (Homebrew's FFmpeg used dav1d too).
#   * zlib comes from the SDK (libz.1.dylib in the shared cache): nothing to ship.
#   * libxml2 (DASH), libsrt + mbedTLS (SRT) static, exactly like Linux; libsrt is
#     C++, so libavformat links the system libc++.
# --disable-autodetect keeps anything else on the build host (Homebrew!) out.
#
# The minimum macOS is the app's LSMinimumSystemVersion (13.0), set for FFmpeg and
# every static dep, so the bundled libs never raise the floor. Native arch only
# (the .pkg ships arm64, built on an arm64 runner).
#
# Result is cached: a stamp keyed on the FFmpeg commit, every dep tag, the target
# this script's own hash and scripts/ffmpeg_components.sh.
#
#   ./scripts/build_ffmpeg_macos.sh            # -> $FFMPEG_PREFIX (default /tmp/ffmpeg-slim-macos)
#
# Needs: Xcode command line tools, git, cmake, meson, ninja, pkg-config.
set -euo pipefail

# Keep FFMPEG_SHA equal to build-windows.yml's pin (FFmpeg 8.x).
FFMPEG_SHA="${FFMPEG_SHA:-e8031e5b9ad2def2a8ff51ca674f708812a7ec29}"
PREFIX="${FFMPEG_PREFIX:-/tmp/ffmpeg-slim-macos}"
WORK="${FFMPEG_WORK:-/tmp/ffmpeg-work-macos}"
SRC="${FFMPEG_SRC:-$WORK/ffmpeg-src-${FFMPEG_SHA:0:8}}"
LIBXML2_TAG="${LIBXML2_TAG:-v2.13.9}"
MBEDTLS_TAG="${MBEDTLS_TAG:-v3.6.5}"
SRT_TAG="${SRT_TAG:-v1.5.4}"
DAV1D_TAG="${DAV1D_TAG:-1.5.1}"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"
ARCH="$(uname -m)"
NETDEPS="$WORK/netdeps-$ARCH-$MACOSX_DEPLOYMENT_TARGET-$LIBXML2_TAG-$MBEDTLS_TAG-$SRT_TAG-dav1d$DAV1D_TAG"
NCPU="$(sysctl -n hw.ncpu)"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=ffmpeg_components.sh
. "$SCRIPT_DIR/ffmpeg_components.sh"
SELF_HASH="$(cat "$0" "$SCRIPT_DIR/ffmpeg_components.sh" "$SCRIPT_DIR"/ffmpeg-patches/*.patch | shasum -a 256 | cut -c1-16)"
STAMP="$FFMPEG_SHA $LIBXML2_TAG $MBEDTLS_TAG $SRT_TAG $DAV1D_TAG $ARCH $MACOSX_DEPLOYMENT_TARGET $SELF_HASH"
if [ -f "$PREFIX/.stamp" ] && [ "$(cat "$PREFIX/.stamp")" = "$STAMP" ]; then
    echo "==> slim FFmpeg cached at $PREFIX ($STAMP)"
    exit 0
fi

for tool in git cmake meson ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "error: $tool not found (brew install $tool)." >&2; exit 1; }
done
mkdir -p "$WORK"

# Static, Release, PIC, deployment target pinned: one helper for the CMake deps.
cmake_static() {   # <src> [-D...]
    local s="$1"; shift
    cmake -S "$s" -B "$s/_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$NETDEPS" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@" >/dev/null
    cmake --build "$s/_build" -j"$NCPU" >/dev/null
    cmake --install "$s/_build" >/dev/null
}

# --- libxml2, static (the dash demuxer's MPD parser, #195) --------------------
# Only what an MPD parse needs, as on Linux / Windows.
if [ ! -f "$NETDEPS/lib/libxml2.a" ]; then
    XMLSRC="$WORK/libxml2-src-$LIBXML2_TAG"
    rm -rf "$XMLSRC"
    git clone -q --depth 1 --branch "$LIBXML2_TAG" https://github.com/GNOME/libxml2.git "$XMLSRC"
    cmake_static "$XMLSRC" \
        -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ICU=OFF \
        -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_FTP=OFF -DLIBXML2_WITH_MODULES=OFF \
        -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF
fi
# --- mbedTLS + libsrt, static (srt://, #198) ------------------------------------
# SRT links only libmbedcrypto (AES, PBKDF2, CTR-DRBG) for passphrase streams.
if [ ! -f "$NETDEPS/lib/libmbedcrypto.a" ]; then
    MBSRC="$WORK/mbedtls-src-$MBEDTLS_TAG"
    rm -rf "$MBSRC"
    git clone -q --depth 1 --branch "$MBEDTLS_TAG" --recurse-submodules --shallow-submodules \
        https://github.com/Mbed-TLS/mbedtls.git "$MBSRC"
    cmake_static "$MBSRC" -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
fi
if [ ! -f "$NETDEPS/lib/libsrt.a" ]; then
    SRTSRC="$WORK/srt-src-$SRT_TAG"
    rm -rf "$SRTSRC"
    git clone -q --depth 1 --branch "$SRT_TAG" https://github.com/Haivision/srt.git "$SRTSRC"
    cmake_static "$SRTSRC" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
        -DENABLE_APPS=OFF -DENABLE_ENCRYPTION=ON -DUSE_ENCLIB=mbedtls \
        -DSSL_INCLUDE_DIRS="$NETDEPS/include" -DSSL_LIBRARY_DIRS="$NETDEPS/lib" \
        -DSSL_LIBRARIES=mbedcrypto
fi
# --- dav1d, static (AV1 software decode) ----------------------------------------
if [ ! -f "$NETDEPS/lib/libdav1d.a" ]; then
    DAVSRC="$WORK/dav1d-src-$DAV1D_TAG"
    rm -rf "$DAVSRC"
    git clone -q --depth 1 --branch "$DAV1D_TAG" https://code.videolan.org/videolan/dav1d.git "$DAVSRC"
    meson setup "$DAVSRC/_build" "$DAVSRC" --buildtype=release --default-library=static \
        --prefix="$NETDEPS" --libdir=lib -Denable_tools=false -Denable_tests=false \
        -Denable_examples=false -Dc_args="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" >/dev/null
    ninja -C "$DAVSRC/_build" >/dev/null
    ninja -C "$DAVSRC/_build" install >/dev/null
fi
# Static-only dir: drop any shared object an install step left, so -L resolves to .a.
rm -f "$NETDEPS"/lib/*.dylib

# pkg-config: ONLY the .pc files written here (PKG_CONFIG_LIBDIR replaces the
# default search path, so Homebrew's libxml-2.0.pc / srt.pc / dav1d.pc can never
# answer instead).
mkdir -p "$NETDEPS/pc"
cat > "$NETDEPS/pc/libxml-2.0.pc" <<EOF
Name: libXML
Description: libxml2 (static, for the dash demuxer)
Version: ${LIBXML2_TAG#v}
Cflags: -I$NETDEPS/include -I$NETDEPS/include/libxml2
Libs: -L$NETDEPS/lib -lxml2 -lm
EOF
cat > "$NETDEPS/pc/srt.pc" <<EOF
Name: srt
Description: libsrt (static, mbedTLS crypto)
Version: ${SRT_TAG#v}
Cflags: -I$NETDEPS/include
Libs: -L$NETDEPS/lib -lsrt -lmbedcrypto -lc++ -lm
EOF
cat > "$NETDEPS/pc/dav1d.pc" <<EOF
Name: dav1d
Description: dav1d (static, AV1 software decode)
Version: $DAV1D_TAG
Cflags: -I$NETDEPS/include
Libs: -L$NETDEPS/lib -ldav1d
EOF
export PKG_CONFIG_LIBDIR="$NETDEPS/pc"
unset PKG_CONFIG_PATH

# --- FFmpeg source at the pinned commit ---------------------------------------
if [ ! -f "$SRC/configure" ]; then
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/FFmpeg/FFmpeg.git
    git -C "$SRC" fetch -q --depth 1 origin "$FFMPEG_SHA"
    git -C "$SRC" checkout -q FETCH_HEAD
fi

# Demuxers / decoders / parsers / bsfs: the list every platform shares.
# Upstream fixes on top of the pin (scripts/ffmpeg-patches, see ffmpeg_components.sh).
ff_apply_patches "$SRC"

BUILD="$SRC/_build-slim-macos"
rm -rf "$BUILD" "$PREFIX"; mkdir -p "$BUILD"; cd "$BUILD"
"$SRC/configure" \
  --prefix="$PREFIX" \
  --enable-shared --disable-static --enable-pic \
  --disable-everything \
  --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter \
  --enable-network --disable-autodetect --disable-debug \
  --enable-swscale --enable-swresample \
  --extra-cflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
  --extra-ldflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
  --enable-zlib --enable-libxml2 --enable-libsrt --enable-libdav1d \
  --enable-securetransport \
  --enable-protocol=file,pipe,tcp,udp,rtp,http,https,tls,rtmp,rtmps,hls,crypto,data,libsrt \
  --enable-demuxer="$FF_DEMUXERS" \
  --enable-decoder="$FF_DECODERS" \
  --enable-parser="$FF_PARSERS" \
  --enable-bsf="$FF_BSFS" \
  --enable-videotoolbox \
  --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,prores_videotoolbox,vp9_videotoolbox,av1_videotoolbox,mpeg1_videotoolbox,mpeg2_videotoolbox,mpeg4_videotoolbox,h263_videotoolbox

# configure silently drops a feature whose dependency is missing: fail instead
# of shipping a player without hardware decode, https, DASH or SRT.
for want in CONFIG_VIDEOTOOLBOX CONFIG_H264_VIDEOTOOLBOX_HWACCEL CONFIG_HEVC_VIDEOTOOLBOX_HWACCEL \
            CONFIG_PRORES_VIDEOTOOLBOX_HWACCEL CONFIG_VP9_VIDEOTOOLBOX_HWACCEL \
            CONFIG_SECURETRANSPORT CONFIG_TLS_PROTOCOL CONFIG_HTTPS_PROTOCOL CONFIG_ZLIB \
            CONFIG_NETWORK CONFIG_RTSP_DEMUXER CONFIG_HLS_DEMUXER CONFIG_UDP_PROTOCOL CONFIG_HTTP_PROTOCOL \
            CONFIG_RTMP_PROTOCOL CONFIG_RTMPS_PROTOCOL \
            CONFIG_EXR_DECODER CONFIG_PNG_DECODER CONFIG_DPX_DECODER CONFIG_PRORES_DECODER CONFIG_DNXHD_DECODER \
            CONFIG_MXF_DEMUXER CONFIG_IMAGE_EXR_PIPE_DEMUXER CONFIG_IMAGE_DPX_PIPE_DEMUXER \
            CONFIG_LIBXML2 CONFIG_DASH_DEMUXER CONFIG_LIBSRT CONFIG_LIBSRT_PROTOCOL \
            CONFIG_LIBDAV1D CONFIG_LIBDAV1D_DECODER CONFIG_JPEG2000_DECODER CONFIG_ALAC_DECODER \
            CONFIG_PCM_S24BE_DECODER CONFIG_RAWVIDEO_DECODER CONFIG_VC1_DECODER CONFIG_PRORES_RAW_DECODER \
            CONFIG_MPJPEG_DEMUXER CONFIG_WEBVTT_DEMUXER CONFIG_DOLBY_E_DECODER; do
    grep -q "^#define $want 1" config.h config_components.h 2>/dev/null || { echo "error: FFmpeg configure disabled $want" >&2; exit 1; }
done

make -j"$NCPU"
make install

# No Mach-O in the prefix may reach Homebrew or any other non-system path: the
# bundler copies only what otool -L lists, so a stray /opt/homebrew dependency here
# would ship as a hard dependency on the user's Homebrew.
for lib in "$PREFIX"/lib/lib{avcodec,avformat,avutil,swscale,swresample}.dylib; do
    bad="$(otool -L "$lib" | tail -n +2 | awk '{print $1}' | grep -vE "^(/usr/lib/|/System/|$PREFIX/lib/)" || true)"
    [ -z "$bad" ] || { echo "error: $lib depends on non-system $bad" >&2; exit 1; }
done

echo "$STAMP" > "$PREFIX/.stamp"
echo "=== slim FFmpeg built at $PREFIX ==="
ls -la "$PREFIX"/lib/*.dylib
