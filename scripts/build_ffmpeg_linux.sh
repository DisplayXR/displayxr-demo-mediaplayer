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
# --disable-autodetect keeps anything else on the build host from sneaking in.
#
# Build it on the OLDEST supported release (the .deb's glibc floor comes from
# here and from the app build). Result is cached: a stamp keyed on the FFmpeg
# commit, the nv-codec-headers tag and this script's own hash.
#
#   ./scripts/build_ffmpeg_linux.sh            # -> $FFMPEG_PREFIX (default /tmp/ffmpeg-slim-linux)
#
# Needs: build-essential, nasm, pkg-config, git, libva-dev, zlib1g-dev.
set -euo pipefail

# Keep FFMPEG_SHA equal to build-windows.yml's pin (FFmpeg 8.x).
FFMPEG_SHA="${FFMPEG_SHA:-e8031e5b9ad2def2a8ff51ca674f708812a7ec29}"
NVCODEC_TAG="${NVCODEC_TAG:-n12.2.72.0}"
PREFIX="${FFMPEG_PREFIX:-/tmp/ffmpeg-slim-linux}"
SRC="${FFMPEG_SRC:-/tmp/ffmpeg-src-${FFMPEG_SHA:0:8}}"
NVSRC="/tmp/nv-codec-headers-$NVCODEC_TAG"

SELF_HASH="$(sha256sum "$0" | cut -c1-16)"
STAMP="$FFMPEG_SHA $NVCODEC_TAG $SELF_HASH"
if [ -f "$PREFIX/.stamp" ] && [ "$(cat "$PREFIX/.stamp")" = "$STAMP" ]; then
    echo "==> slim FFmpeg cached at $PREFIX ($STAMP)"
    exit 0
fi

command -v nasm >/dev/null || { echo "error: nasm not found (needed for FFmpeg's x86 SIMD)." >&2; exit 1; }
pkg-config --exists libva libva-drm || { echo "error: libva-dev not found (VAAPI hwaccel)." >&2; exit 1; }

# --- nv-codec-headers (header-only, installs ffnvcodec.pc) -------------------
if [ ! -f "$NVSRC/Makefile" ]; then
    rm -rf "$NVSRC"
    git clone --depth 1 --branch "$NVCODEC_TAG" \
        https://github.com/FFmpeg/nv-codec-headers.git "$NVSRC"
fi
rm -rf "$PREFIX"
make -C "$NVSRC" PREFIX="$PREFIX" install >/dev/null
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- FFmpeg source at the pinned commit ---------------------------------------
if [ ! -f "$SRC/configure" ]; then
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/FFmpeg/FFmpeg.git
    git -C "$SRC" fetch -q --depth 1 origin "$FFMPEG_SHA"
    git -C "$SRC" checkout -q FETCH_HEAD
fi

BUILD="$SRC/_build-slim-linux"
rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"
"$SRC/configure" \
  --prefix="$PREFIX" \
  --enable-shared --disable-static --enable-pic \
  --disable-everything \
  --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter \
  --disable-network --disable-autodetect --disable-debug \
  --enable-swscale --enable-swresample \
  --enable-zlib \
  --enable-protocol=file,pipe \
  --enable-demuxer=mov,matroska,mpegts,avi,flv,wav,flac,ogg,mp3,aac,ac3,eac3,mjpeg,image2,image2pipe,h264,hevc,av1,m4v \
  --enable-decoder=h264,hevc,av1,vp9,vp8,mpeg4,mpeg2video,mpeg1video,mjpeg,png,bmp,tiff,webp,gif,aac,aac_latm,mp3,ac3,eac3,flac,opus,vorbis,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8 \
  --enable-parser=h264,hevc,av1,vp9,vp8,mpeg4video,mpegvideo,aac,aac_latm,ac3,flac,opus,vorbis,mjpeg,png \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,av1_frame_split,vp9_superframe,vp9_superframe_split,aac_adtstoasc,extract_extradata,mpeg4_unpack_bframes,null \
  --enable-vaapi \
  --enable-ffnvcodec --enable-cuda --enable-nvdec \
  --enable-hwaccel=h264_vaapi,hevc_vaapi,av1_vaapi,vp9_vaapi,vp8_vaapi,mpeg2_vaapi,mpeg4_vaapi,h264_nvdec,hevc_nvdec,av1_nvdec,vp9_nvdec,vp8_nvdec,mpeg2_nvdec,mpeg4_nvdec

# configure silently drops a feature whose dependency is missing: fail instead
# of shipping a player without hardware decode.
for want in CONFIG_VAAPI CONFIG_NVDEC CONFIG_H264_VAAPI_HWACCEL CONFIG_HEVC_NVDEC_HWACCEL CONFIG_ZLIB; do
    # (component switches live in config_components.h on FFmpeg >= 6)
    cat config.h config_components.h 2>/dev/null | grep -q "^#define $want 1" || { echo "error: FFmpeg configure disabled $want" >&2; exit 1; }
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
