#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the PRIVATE, slim, DECODE-ONLY FFmpeg that the Linux .deb bundles (#76).
#
# Why not the distro FFmpeg: its sonames change on every Ubuntu release
# (22.04 = libavcodec58, 24.04 = libavcodec60, 26.04 = libavcodec62), so a .deb
# linked against the build host's libav* is uninstallable everywhere else. The
# player instead ships its own FFmpeg next to the binary (found via
# RUNPATH=$ORIGIN), exactly like the Windows installer ships its slim DLLs
# (scripts/build-ffmpeg-slim.sh). Shared, not static, so the LGPL relinking
# story is the same as on Windows.
#
# Same FFmpeg commit, same component lists as the Windows build. Differences are
# only the Linux hwaccels and the toolchain:
#   * VAAPI (Intel/AMD) — links libva.so.2 + libva-drm.so.2 (packages libva2 /
#     libva-drm2: same names on 22.04, 24.04 and 26.04). DRM render node only, no
#     libva-x11: av_hwdevice_ctx_create(VAAPI, NULL) falls back to /dev/dri.
#   * NVDEC (NVIDIA) — via the header-only nv-codec-headers, which dlopen()
#     libcuda/libnvcuvid at runtime: no DT_NEEDED, no build- or run-time CUDA dep.
#   * zlib — libz.so.1 (zlib1g, stable everywhere).
#   * Network input (#93): the same protocols/demuxers as Windows (RTSP/RTP/SDP,
#     RTMP, UDP, HTTP, HLS) but NO TLS. Every Linux TLS backend adds a DT_NEEDED
#     whose PACKAGE differs across 22.04/24.04/26.04 (the 24.04 64-bit time_t
#     transition renamed libssl3 -> libssl3t64 and libgnutls30 -> libgnutls30t64;
#     mbedtls changes soname), which breaks the .deb's one-package rule
#     (package_deb_linux.sh STABLE_SONAMES). So on Linux https://, rtmps:// and
#     HLS-over-https are unavailable; plain http / rtsp / rtmp / udp / rtp work.
#   * DASH (#195): libxml2, built here and linked STATICALLY into libavformat.
#     A shared libxml2 would break the same rule: 26.04's libxml2 2.14 moved to
#     soname libxml2.so.16 (package libxml2-16) while 22.04/24.04 have .so.2.
#   * SRT (#198): libsrt + mbedTLS (its AES crypto), both STATIC for the same
#     reason (distro libsrt / libmbedcrypto sonames and t64 package names differ
#     across the three releases). libsrt is C++, so libavformat gains a
#     libstdc++.so.6 DT_NEEDED, which is on the .deb's STABLE_SONAMES list.
#   * AV1 software decode (#201): libdav1d, STATIC (distro libdav1d is .so.5 on
#     22.04, .so.7 on 24.04 / 26.04). FFmpeg's native av1 decoder is hardware-only,
#     so without it AV1 did not play on a GPU without AV1 decode.
# --disable-autodetect keeps anything else on the build host from sneaking in.
#
# Build it on the OLDEST supported release (the .deb's glibc floor comes from
# here and from the app build). Result is cached: a stamp keyed on the FFmpeg
# commit, the nv-codec-headers tag and this script's own hash.
#
#   ./scripts/build_ffmpeg_linux.sh            # -> $FFMPEG_PREFIX (default /tmp/ffmpeg-slim-linux)
#
# Needs: build-essential, nasm, pkg-config, git, cmake, meson, ninja-build, libva-dev, zlib1g-dev.
set -euo pipefail

# Keep FFMPEG_SHA equal to build-windows.yml's pin (FFmpeg 8.x).
FFMPEG_SHA="${FFMPEG_SHA:-e8031e5b9ad2def2a8ff51ca674f708812a7ec29}"
NVCODEC_TAG="${NVCODEC_TAG:-n12.2.72.0}"
PREFIX="${FFMPEG_PREFIX:-/tmp/ffmpeg-slim-linux}"
SRC="${FFMPEG_SRC:-/tmp/ffmpeg-src-${FFMPEG_SHA:0:8}}"
NVSRC="/tmp/nv-codec-headers-$NVCODEC_TAG"
LIBXML2_TAG="${LIBXML2_TAG:-v2.13.9}"
MBEDTLS_TAG="${MBEDTLS_TAG:-v3.6.5}"
SRT_TAG="${SRT_TAG:-v1.5.4}"
DAV1D_TAG="${DAV1D_TAG:-1.5.1}"
NETDEPS="/tmp/ffmpeg-netdeps-linux-$LIBXML2_TAG-$MBEDTLS_TAG-$SRT_TAG-dav1d$DAV1D_TAG"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Demuxers / decoders / parsers / bsfs: the list every platform shares (#201).
# shellcheck source=ffmpeg_components.sh
. "$SCRIPT_DIR/ffmpeg_components.sh"
SELF_HASH="$(cat "$0" "$SCRIPT_DIR/ffmpeg_components.sh" "$SCRIPT_DIR"/ffmpeg-patches/*.patch | sha256sum | cut -c1-16)"
STAMP="$FFMPEG_SHA $NVCODEC_TAG $LIBXML2_TAG $MBEDTLS_TAG $SRT_TAG $DAV1D_TAG $SELF_HASH"
if [ -f "$PREFIX/.stamp" ] && [ "$(cat "$PREFIX/.stamp")" = "$STAMP" ]; then
    echo "==> slim FFmpeg cached at $PREFIX ($STAMP)"
    exit 0
fi

command -v nasm >/dev/null || { echo "error: nasm not found (needed for FFmpeg's x86 SIMD)." >&2; exit 1; }
pkg-config --exists libva libva-drm || { echo "error: libva-dev not found (VAAPI hwaccel)." >&2; exit 1; }
command -v cmake >/dev/null || { echo "error: cmake not found (builds the static libxml2)." >&2; exit 1; }
{ command -v meson && command -v ninja; } >/dev/null || { echo "error: meson + ninja not found (build the static dav1d)." >&2; exit 1; }

# --- nv-codec-headers (header-only, installs ffnvcodec.pc) -------------------
if [ ! -f "$NVSRC/Makefile" ]; then
    rm -rf "$NVSRC"
    git clone --depth 1 --branch "$NVCODEC_TAG" \
        https://github.com/FFmpeg/nv-codec-headers.git "$NVSRC"
fi
rm -rf "$PREFIX"
make -C "$NVSRC" PREFIX="$PREFIX" install >/dev/null
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- libxml2, static + PIC (the dash demuxer's MPD parser, #195) --------------
# Only what an MPD parse needs: no iconv (MPDs are UTF-8), no zlib/lzma/ICU, no
# HTTP/FTP, no programs/tests/python. The .pc is written here so configure links
# the archive (and only it) — never the build host's libxml2.so: NETDEPS/lib holds
# only libxml2.a, so -L there + -lxml2 resolves to it. (Not the archive's full path:
# configure treats a non -l/-L word as a FLAG and puts it before the probe's object,
# where the linker never uses it.)
if [ ! -f "$NETDEPS/lib/libxml2.a" ]; then
    XMLSRC="/tmp/libxml2-src-$LIBXML2_TAG"
    rm -rf "$XMLSRC"
    git clone -q --depth 1 --branch "$LIBXML2_TAG" https://github.com/GNOME/libxml2.git "$XMLSRC"
    cmake -S "$XMLSRC" -B "$XMLSRC/_build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$NETDEPS" -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ICU=OFF \
        -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_FTP=OFF -DLIBXML2_WITH_MODULES=OFF \
        -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF >/dev/null
    cmake --build "$XMLSRC/_build" -j"$(nproc)" >/dev/null
    cmake --install "$XMLSRC/_build" >/dev/null
fi
# --- mbedTLS + libsrt, static + PIC (srt://, #198) ------------------------------
# SRT links only libmbedcrypto (AES, PBKDF2, CTR-DRBG) for passphrase streams.
if [ ! -f "$NETDEPS/lib/libmbedcrypto.a" ]; then
    MBSRC="/tmp/mbedtls-src-$MBEDTLS_TAG"
    rm -rf "$MBSRC"
    git clone -q --depth 1 --branch "$MBEDTLS_TAG" --recurse-submodules --shallow-submodules \
        https://github.com/Mbed-TLS/mbedtls.git "$MBSRC"
    cmake -S "$MBSRC" -B "$MBSRC/_build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$NETDEPS" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF >/dev/null
    cmake --build "$MBSRC/_build" -j"$(nproc)" >/dev/null
    cmake --install "$MBSRC/_build" >/dev/null
fi
if [ ! -f "$NETDEPS/lib/libsrt.a" ]; then
    SRTSRC="/tmp/srt-src-$SRT_TAG"
    rm -rf "$SRTSRC"
    git clone -q --depth 1 --branch "$SRT_TAG" https://github.com/Haivision/srt.git "$SRTSRC"
    cmake -S "$SRTSRC" -B "$SRTSRC/_build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_INSTALL_PREFIX="$NETDEPS" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
        -DENABLE_APPS=OFF -DENABLE_ENCRYPTION=ON -DUSE_ENCLIB=mbedtls \
        -DSSL_INCLUDE_DIRS="$NETDEPS/include" -DSSL_LIBRARY_DIRS="$NETDEPS/lib" \
        -DSSL_LIBRARIES=mbedcrypto >/dev/null
    cmake --build "$SRTSRC/_build" -j"$(nproc)" >/dev/null
    cmake --install "$SRTSRC/_build" >/dev/null
fi
# --- dav1d, static + PIC (AV1 software decode, #201) ---------------------------
if [ ! -f "$NETDEPS/lib/libdav1d.a" ]; then
    DAVSRC="/tmp/dav1d-src-$DAV1D_TAG"
    rm -rf "$DAVSRC"
    git clone -q --depth 1 --branch "$DAV1D_TAG" https://code.videolan.org/videolan/dav1d.git "$DAVSRC"
    meson setup "$DAVSRC/_build" "$DAVSRC" --buildtype=release --default-library=static \
        --prefix="$NETDEPS" --libdir=lib -Db_staticpic=true \
        -Denable_tools=false -Denable_tests=false -Denable_examples=false >/dev/null
    ninja -C "$DAVSRC/_build" >/dev/null
    ninja -C "$DAVSRC/_build" install >/dev/null
fi
# Static-only dir: drop any shared object an install step left, so -L resolves to .a.
rm -f "$NETDEPS"/lib/*.so*

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
Libs: -L$NETDEPS/lib -lsrt -lmbedcrypto -lstdc++ -lpthread -lm
EOF
cat > "$NETDEPS/pc/dav1d.pc" <<EOF
Name: dav1d
Description: dav1d (static, AV1 software decode)
Version: $DAV1D_TAG
Cflags: -I$NETDEPS/include
Libs: -L$NETDEPS/lib -ldav1d -lpthread -lm
EOF
export PKG_CONFIG_PATH="$NETDEPS/pc:$PKG_CONFIG_PATH"

# --- FFmpeg source at the pinned commit ---------------------------------------
if [ ! -f "$SRC/configure" ]; then
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/FFmpeg/FFmpeg.git
    git -C "$SRC" fetch -q --depth 1 origin "$FFMPEG_SHA"
    git -C "$SRC" checkout -q FETCH_HEAD
fi

# Upstream fixes on top of the pin (scripts/ffmpeg-patches, see ffmpeg_components.sh).
ff_apply_patches "$SRC"

BUILD="$SRC/_build-slim-linux"
rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"
"$SRC/configure" \
  --prefix="$PREFIX" \
  --enable-shared --disable-static --enable-pic \
  --disable-everything \
  --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter \
  --enable-network --disable-autodetect --disable-debug \
  --enable-swscale --enable-swresample \
  --enable-zlib --enable-libxml2 --enable-libsrt --enable-libdav1d \
  --enable-protocol=file,pipe,tcp,udp,rtp,http,rtmp,hls,crypto,data,libsrt \
  --enable-demuxer="$FF_DEMUXERS" \
  --enable-decoder="$FF_DECODERS" \
  --enable-parser="$FF_PARSERS" \
  --enable-bsf="$FF_BSFS" \
  --enable-vaapi \
  --enable-ffnvcodec --enable-cuda --enable-nvdec \
  --enable-hwaccel=h264_vaapi,hevc_vaapi,av1_vaapi,vp9_vaapi,vp8_vaapi,mpeg2_vaapi,mpeg4_vaapi,h264_nvdec,hevc_nvdec,av1_nvdec,vp9_nvdec,vp8_nvdec,mpeg2_nvdec,mpeg4_nvdec

# configure silently drops a feature whose dependency is missing: fail instead
# of shipping a player without hardware decode.
for want in CONFIG_VAAPI CONFIG_NVDEC CONFIG_H264_VAAPI_HWACCEL CONFIG_HEVC_NVDEC_HWACCEL CONFIG_ZLIB \
            CONFIG_NETWORK CONFIG_RTSP_DEMUXER CONFIG_HLS_DEMUXER CONFIG_UDP_PROTOCOL CONFIG_HTTP_PROTOCOL \
            CONFIG_EXR_DECODER CONFIG_PNG_DECODER CONFIG_DPX_DECODER CONFIG_PRORES_DECODER CONFIG_DNXHD_DECODER \
            CONFIG_MXF_DEMUXER CONFIG_IMAGE_EXR_PIPE_DEMUXER CONFIG_IMAGE_DPX_PIPE_DEMUXER \
            CONFIG_LIBXML2 CONFIG_DASH_DEMUXER CONFIG_LIBSRT CONFIG_LIBSRT_PROTOCOL \
            CONFIG_LIBDAV1D_DECODER CONFIG_VC1_DECODER CONFIG_JPEG2000_DECODER CONFIG_MPJPEG_DEMUXER CONFIG_WEBVTT_DEMUXER; do
    # (component switches live in config_components.h on FFmpeg >= 6). grep the files
    # directly: `cat ... | grep -q` under pipefail fails when grep -q exits on an early
    # match and cat dies of SIGPIPE (CONFIG_LIBXML2 sits near the top of config.h).
    grep -q "^#define $want 1" config.h config_components.h 2>/dev/null || { echo "error: FFmpeg configure disabled $want" >&2; exit 1; }
done

# RUNPATH=$ORIGIN on each lib, so libavformat finds the bundled libavcodec even
# where the binary's own RUNPATH does not apply (RUNPATH is not transitive).
# Passed at make time, not via --extra-ldsoflags: configure eval()s its flags and
# eats the '$'. The library link line is make-expanded TWICE, hence `\$$$$`
# ($$$$ -> $$ -> $, then the recipe shell turns \$ into $);
# the check after install proves it.
make -j"$(nproc)" LDSOFLAGS='-Wl,-rpath,\$$$$ORIGIN'
make install LDSOFLAGS='-Wl,-rpath,\$$$$ORIGIN'

# Verify each lib carries RUNPATH=$ORIGIN.
for so in "$PREFIX"/lib/lib{avcodec,avformat,avutil,swscale,swresample}.so; do
    objdump -p "$so" | grep -q 'RUNPATH *\$ORIGIN$' || { echo "error: $so lacks RUNPATH \$ORIGIN" >&2; objdump -p "$so" | grep -E 'R(UN)?PATH' >&2; exit 1; }
done

echo "$STAMP" > "$PREFIX/.stamp"
echo "=== slim FFmpeg built at $PREFIX ==="
ls -la "$PREFIX"/lib/*.so.*
