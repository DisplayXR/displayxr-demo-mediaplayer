#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build a minimal, DECODE-ONLY FFmpeg (MSVC toolchain) for the DisplayXR media
# player. A stereo player only needs to *decode* H.264/H.265/AV1/VP8/VP9 (+ common
# audio), so we strip every encoder and external library that a stock "full" build
# statically links (x264/x265/aom/svt-av1/rav1e/vpx/vvenc/kvazaar, libass,
# libbluray, libplacebo, whisper, openal, vmaf, shaderc, …). Result: the 5 shared
# libs (avcodec/avformat/avutil/swscale/swresample) total ~9 MB vs ~103 MB for a
# stock full build — cutting the demo's installer from ~46 MB to ~12 MB.
#
# HW decode is kept via D3D11VA + DXVA2 (covers Intel/AMD/NVIDIA on Windows);
# x86 SIMD stays on (nasm) so software fallback isn't crippled. AV1 software
# fallback uses FFmpeg's native decoder (no libdav1d) — only slower on GPUs that
# lack AV1 hardware decode.
#
# Network input (#93, the live stream-URL source): network + the protocols and
# demuxers a live stream needs — RTSP (+ RTP/SDP depacketizers), RTMP(S), UDP/RTP
# MPEG-TS, HTTP(S) progressive and HLS. TLS (https://, rtmps://, HLS over https)
# uses SChannel: a Windows system API (secur32/ncrypt/crypt32), so no new DLL
# ships. SRT is deliberately absent (it would need libsrt).
#
# Requirements: run under a POSIX shell (MSYS2 / Git bash) with MSVC `cl` and
# `nasm` on PATH and GNU `make` available, FFmpeg source already checked out.
#
#   FFMPEG_SRC=/path/to/ffmpeg-src  FFMPEG_PREFIX=/path/to/out  ./build-ffmpeg-slim.sh
set -euo pipefail

SRC="${FFMPEG_SRC:?set FFMPEG_SRC to the FFmpeg source dir}"
PREFIX="${FFMPEG_PREFIX:?set FFMPEG_PREFIX to the install prefix}"

# The MSVC link.exe/cl.exe must win over the shell's /usr/bin/link.exe (the classic
# FFmpeg-on-Windows collision). Put the MSVC tools dir (where cl lives) first.
clpath="$(command -v cl || true)"
[ -n "$clpath" ] || { echo "ERROR: cl (MSVC) not on PATH — run inside an MSVC dev environment"; exit 1; }
export PATH="$(dirname "$clpath"):$PATH"
echo "cl=$(command -v cl)"
echo "link=$(command -v link)"
echo "nasm=$(command -v nasm || echo MISSING)"
echo "make=$(command -v make || echo MISSING)"

cd "$SRC"
./configure \
  --toolchain=msvc \
  --prefix="$PREFIX" \
  --enable-shared --disable-static \
  --disable-everything \
  --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter \
  --enable-network --disable-autodetect --disable-debug \
  --enable-swscale --enable-swresample \
  --enable-protocol=file,pipe,tcp,udp,rtp,http,https,tls,rtmp,rtmps,hls,crypto,data \
  --enable-demuxer=mov,matroska,mpegts,avi,flv,live_flv,rtsp,rtp,sdp,hls,wav,flac,ogg,mp3,aac,ac3,eac3,mjpeg,image2,image2pipe,h264,hevc,av1,m4v \
  --enable-decoder=h264,hevc,av1,vp9,vp8,mpeg4,mpeg2video,mpeg1video,mjpeg,png,bmp,tiff,webp,gif,aac,aac_latm,mp3,ac3,eac3,flac,opus,vorbis,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8 \
  --enable-parser=h264,hevc,av1,vp9,vp8,mpeg4video,mpegvideo,aac,aac_latm,ac3,flac,opus,vorbis,mjpeg,png \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,av1_frame_split,vp9_superframe,vp9_superframe_split,aac_adtstoasc,extract_extradata,mpeg4_unpack_bframes,null \
  --enable-hwaccel=h264_d3d11va,h264_d3d11va2,h264_dxva2,hevc_d3d11va,hevc_d3d11va2,hevc_dxva2,av1_d3d11va,av1_d3d11va2,av1_dxva2,vp9_d3d11va,vp9_d3d11va2,vp9_dxva2 \
  --enable-d3d11va --enable-dxva2 \
  --enable-schannel

# configure silently drops a feature whose dependency is missing (schannel is an
# explicit enable under --disable-autodetect and soft-disables when its headers or
# libs are absent): fail instead of shipping a player whose https:// is dead.
for want in CONFIG_NETWORK CONFIG_SCHANNEL CONFIG_TLS_PROTOCOL CONFIG_HTTPS_PROTOCOL \
            CONFIG_RTSP_DEMUXER CONFIG_HLS_DEMUXER CONFIG_UDP_PROTOCOL CONFIG_RTMP_PROTOCOL; do
    # (component switches live in config_components.h on FFmpeg >= 6)
    cat config.h config_components.h 2>/dev/null | grep -q "^#define $want 1" || { echo "ERROR: FFmpeg configure disabled $want" >&2; exit 1; }
done

make -j"$(nproc)"
make install

# The MSVC shared build drops the import libs (*.lib) next to the DLLs in bin/.
# The app's CMake find_library() looks in lib/, so mirror them there to make the
# prefix a standard FFMPEG_ROOT dev tree (include/ + lib/ + bin/).
mkdir -p "$PREFIX/lib"
cp -f "$PREFIX"/bin/*.lib "$PREFIX/lib/" 2>/dev/null || true

echo "=== slim FFmpeg built at $PREFIX ==="
ls -la "$PREFIX/bin/"*.dll
