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
# x86 SIMD stays on (nasm) so software fallback isn't crippled. AV1 needs D3D11VA:
# FFmpeg's native av1 decoder is hardware-only and this build has no libdav1d (the
# macOS and Linux builds link it statically, #201), so AV1 does not play on a GPU
# without AV1 decode.
#
# Network input (#93, the live stream-URL source): network + the protocols and
# demuxers a live stream needs — RTSP (+ RTP/SDP depacketizers), RTMP(S), UDP/RTP
# MPEG-TS, HTTP(S) progressive, HLS and DASH (#195). TLS (https://, rtmps://, HLS /
# DASH over https) uses SChannel: a Windows system API (secur32/ncrypt/crypt32), so
# no new DLL ships. DASH needs libxml2 and SRT (#198) needs libsrt + a crypto
# library (mbedTLS); all three are built here statically like zlib (below).
#
# Requirements: run under a POSIX shell (MSYS2 / Git bash) with MSVC `cl`, `nmake`,
# `cmake` (the VS developer environment has both) and `nasm` on PATH and GNU `make`
# available, FFmpeg source already checked out. No pkg-config is needed: the static
# deps are fed to configure through a tiny shim (see netdeps below).
#
#   FFMPEG_SRC=/path/to/ffmpeg-src  FFMPEG_PREFIX=/path/to/out  ./build-ffmpeg-slim.sh
set -euo pipefail

SRC="${FFMPEG_SRC:?set FFMPEG_SRC to the FFmpeg source dir}"
# Demuxers / decoders / parsers / bsfs: the list every platform shares (#201).
# shellcheck source=ffmpeg_components.sh
. "$(cd "$(dirname "$0")" && pwd)/ffmpeg_components.sh"
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

# zlib, static, built here for MSVC: FFmpeg's exr and png decoders depend on it and
# configure DROPS them silently when it is missing (under --disable-autodetect zlib is
# off unless enabled, and an enabled zlib with no library soft-disables). Linked into
# avcodec: no new DLL in the installer. Pinned; the cache key hashes this script.
ZLIB_TAG=v1.3.1
ZLIB_SRC="$(cd "$SRC/.." && pwd)/zlib-src"
ZLIB_PREFIX="$(cd "$SRC/.." && pwd)/zlib-msvc"
if [ ! -f "$ZLIB_PREFIX/lib/z.lib" ]; then
    rm -rf "$ZLIB_SRC" "$ZLIB_PREFIX"
    git clone --depth 1 --branch "$ZLIB_TAG" https://github.com/madler/zlib.git "$ZLIB_SRC"
    ( cd "$ZLIB_SRC" && nmake -f win32/Makefile.msc zlib.lib )   # -MD, like FFmpeg's msvc build
    mkdir -p "$ZLIB_PREFIX/include" "$ZLIB_PREFIX/lib"
    cp "$ZLIB_SRC/zlib.h" "$ZLIB_SRC/zconf.h" "$ZLIB_PREFIX/include/"
    # FFmpeg's config.h defines HAVE_UNISTD_H to 0 on MSVC, and zconf.h tests it with
    # #ifdef, so every FFmpeg file that includes zlib.h would pull in unistd.h (CI:
    # "zconf.h(484): cannot open include file 'unistd.h'"). Make the test a value test.
    sed -i 's|^#ifdef HAVE_UNISTD_H |#if defined(HAVE_UNISTD_H) \&\& HAVE_UNISTD_H |' "$ZLIB_PREFIX/include/zconf.h"
    grep -q "defined(HAVE_UNISTD_H) && HAVE_UNISTD_H" "$ZLIB_PREFIX/include/zconf.h" || { echo "ERROR: zconf.h HAVE_UNISTD_H patch did not apply" >&2; exit 1; }
    # configure's msvc toolchain maps -lz to zlib.lib (its one special case; every other
    # -lfoo becomes foo.lib), so ship both names.
    cp "$ZLIB_SRC/zlib.lib" "$ZLIB_PREFIX/lib/zlib.lib"
    cp "$ZLIB_SRC/zlib.lib" "$ZLIB_PREFIX/lib/z.lib"
fi
# Mixed form (D:/a/...): a backslash path survives configure's probes but not
# config.mak -> make -> sh, where "\a\_temp" is an escape sequence (CI: http.c could
# not find zlib.h after configure had found it). cl and link take forward slashes.
ZLIB_INC_W="$(cygpath -m "$ZLIB_PREFIX/include" 2>/dev/null || echo "$ZLIB_PREFIX/include")"
ZLIB_LIB_W="$(cygpath -m "$ZLIB_PREFIX/lib" 2>/dev/null || echo "$ZLIB_PREFIX/lib")"
echo "zlib=$ZLIB_PREFIX ($ZLIB_TAG)"

# --- netdeps: static libraries for the network demuxers (#195) ----------------------
# libxml2 (FFmpeg's dash demuxer parses the MPD with it). Static, linked into avformat:
# no new DLL to ship or sign. /MT, the CRT FFmpeg's msvc build uses (configure passes
# no -MD, so cl defaults to the static CRT); a /MD archive leaves __imp_fwrite & co.
# unresolved in configure's link probe and libxml2 is then "not found". Only what an MPD parse needs: no iconv (an MPD
# is UTF-8; libxml2 keeps its built-in UTF-8/16 + Latin-1 decoders), no zlib/lzma, no
# HTTP/FTP, no programs/tests/python. Pinned; the cache key hashes this script.
#
# FFmpeg's configure finds libxml2 and libsrt through pkg-config. The MSYS2 on the CI images and
# the signing box need not have one, and a host pkg-config could leak a foreign .pc in,
# so configure gets --pkg-config=<shim>: it answers ONLY for the .pc files written
# here (Cflags / Libs verbatim, which msvc's flag filter maps: -L -> -libpath:, -lx ->
# x.lib) and fails every other query.
LIBXML2_TAG=v2.13.9
MBEDTLS_TAG=v3.6.5
SRT_TAG=v1.5.4
command -v cmake >/dev/null || { echo "ERROR: cmake not on PATH (it ships with the VS developer environment)" >&2; exit 1; }
NETDEPS_PREFIX="$(cd "$SRC/.." && pwd)/netdeps-msvc-$LIBXML2_TAG-$MBEDTLS_TAG-$SRT_TAG"
NETDEPS_W="$(cygpath -m "$NETDEPS_PREFIX" 2>/dev/null || echo "$NETDEPS_PREFIX")"
# cmake_msvc <src-dir> <build-dir> [-D...]: static Release build + install into NETDEPS.
cmake_msvc() {
    local s b
    s="$(cygpath -m "$1" 2>/dev/null || echo "$1")"; b="$(cygpath -m "$2" 2>/dev/null || echo "$2")"; shift 2
    cmake -S "$s" -B "$b" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$NETDEPS_W" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
    cmake --build "$b"
    cmake --install "$b"
}
if [ ! -f "$NETDEPS_PREFIX/lib/xml2.lib" ]; then
    LIBXML2_SRC="$(cd "$SRC/.." && pwd)/libxml2-src"
    rm -rf "$LIBXML2_SRC"
    git clone --depth 1 --branch "$LIBXML2_TAG" https://github.com/GNOME/libxml2.git "$LIBXML2_SRC"
    cmake_msvc "$LIBXML2_SRC" "$LIBXML2_SRC/_build" \
        -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_LZMA=OFF \
        -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_FTP=OFF -DLIBXML2_WITH_MODULES=OFF \
        -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF
    # The static MSVC library is libxml2s.lib; configure's msvc filter maps -lxml2 to
    # xml2.lib, so publish that name.
    xmllib="$(ls "$NETDEPS_PREFIX"/lib/*xml2*.lib | head -1)"
    [ -n "$xmllib" ] || { echo "ERROR: libxml2 build produced no .lib" >&2; exit 1; }
    cp "$xmllib" "$NETDEPS_PREFIX/lib/xml2.lib"
fi
# libsrt (#198), static, with mbedTLS for its AES encryption (passphrase streams).
# Why mbedTLS: SRT needs a crypto library for encryption, and the alternatives are
# OpenSSL (heavy, and a second TLS stack next to SChannel) or encryption off (no
# passphrase streams, the common case for contribution feeds). mbedTLS builds cleanly
# with MSVC through CMake, and SRT only links its libmbedcrypto (AES, SHA-1 PBKDF2,
# CTR-DRBG). SRT on MSVC uses C++11 threads (ENABLE_STDCXX_SYNC), so no pthreads-w32.
# Its C++ runtime is static (/MT) like the rest: avformat stays a self-contained DLL.
if [ ! -f "$NETDEPS_PREFIX/lib/mbedcrypto.lib" ]; then
    MBEDTLS_SRC="$(cd "$SRC/.." && pwd)/mbedtls-src"
    rm -rf "$MBEDTLS_SRC"
    git clone --depth 1 --branch "$MBEDTLS_TAG" --recurse-submodules --shallow-submodules \
        https://github.com/Mbed-TLS/mbedtls.git "$MBEDTLS_SRC"
    cmake_msvc "$MBEDTLS_SRC" "$MBEDTLS_SRC/_build" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
fi
if [ ! -f "$NETDEPS_PREFIX/lib/srt.lib" ]; then
    SRT_SRC="$(cd "$SRC/.." && pwd)/srt-src"
    rm -rf "$SRT_SRC"
    git clone --depth 1 --branch "$SRT_TAG" https://github.com/Haivision/srt.git "$SRT_SRC"
    # SSL_* given directly: SRT's FindMbedTLS would otherwise go looking on its own.
    cmake_msvc "$SRT_SRC" "$SRT_SRC/_build" \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_APPS=OFF -DENABLE_ENCRYPTION=ON \
        -DUSE_ENCLIB=mbedtls -DSSL_INCLUDE_DIRS="$NETDEPS_W/include" \
        -DSSL_LIBRARY_DIRS="$NETDEPS_W/lib" -DSSL_LIBRARIES=mbedcrypto
    # SRT names its static MSVC library srt_static.lib; configure maps -lsrt to srt.lib.
    [ -f "$NETDEPS_PREFIX/lib/srt_static.lib" ] || { echo "ERROR: libsrt build produced no srt_static.lib" >&2; exit 1; }
    cp "$NETDEPS_PREFIX/lib/srt_static.lib" "$NETDEPS_PREFIX/lib/srt.lib"
fi
mkdir -p "$NETDEPS_PREFIX/pc"
# LIBXML_STATIC: without it the headers declare every symbol __declspec(dllimport).
cat > "$NETDEPS_PREFIX/pc/libxml-2.0.pc" <<EOF
Version: ${LIBXML2_TAG#v}
Cflags: -I$NETDEPS_W/include -I$NETDEPS_W/include/libxml2 -DLIBXML_STATIC
Libs: -L$NETDEPS_W/lib -lxml2 -lws2_32 -lbcrypt
EOF
cat > "$NETDEPS_PREFIX/pc/srt.pc" <<EOF
Version: ${SRT_TAG#v}
Cflags: -I$NETDEPS_W/include
Libs: -L$NETDEPS_W/lib -lsrt -lmbedcrypto -lws2_32 -lbcrypt
EOF
PKGCONF_SHIM="$NETDEPS_PREFIX/pc/pkg-config-shim.sh"
cat > "$PKGCONF_SHIM" <<'EOF'
#!/bin/sh
# Minimal pkg-config for FFmpeg's configure: answers from the .pc files next to it.
dir="$(cd "$(dirname "$0")" && pwd)"
mode=; pkg=
for a in "$@"; do
    case "$a" in
        --version) echo 0.29.2; exit 0 ;;
        --exists) mode=exists ;;
        --cflags|--cflags-only-I) mode=cflags ;;
        --libs) mode=libs ;;
        --modversion) mode=version ;;
        --variable=*) mode=variable ;;
        -*) ;;
        *) [ -n "$pkg" ] || pkg="${a%% *}" ;;
    esac
done
f="$dir/$pkg.pc"
[ -n "$pkg" ] && [ -f "$f" ] || { echo "Package $pkg was not found (pkg-config shim)" >&2; exit 1; }
case "$mode" in
    exists|variable) ;;
    cflags) sed -n 's/^Cflags: //p' "$f" ;;
    libs) sed -n 's/^Libs: //p' "$f" ;;
    version) sed -n 's/^Version: //p' "$f" ;;
esac
EOF
chmod +x "$PKGCONF_SHIM"
echo "netdeps=$NETDEPS_PREFIX (libxml2 $LIBXML2_TAG, mbedtls $MBEDTLS_TAG, srt $SRT_TAG)"

cd "$SRC"

# configure (at the pinned commit and on master, 2026-09-30) hands compile-only
# -D defines to the LINKER in test_ld, and since ~2024 it treats link.exe's
# "LNK4044: unrecognized option" warning as a failed probe. The schannel probe
# (`-DSECURITY_WIN32 -lsecur32`) therefore always fails under --toolchain=msvc:
# "schannel requested but not found" (#93; the four probes pass when run by hand).
# Drop -D* from the linker's flags — defines mean nothing to a linker anywhere.
if ! grep -q "filter_out '-D\*' \$flags" configure; then
    sed -i "s|^    flags=\$(\$ldflags_filter \$flags)\$|    flags=\$(\$ldflags_filter \$(filter_out '-D*' \$flags))|" configure
    grep -q "filter_out '-D\*' \$flags" configure || { echo "ERROR: test_ld patch did not apply (configure changed upstream?)" >&2; exit 1; }
    echo "patched configure: test_ld drops -D* from linker flags"
fi
./configure \
  --toolchain=msvc \
  --prefix="$PREFIX" \
  --enable-shared --disable-static \
  --disable-everything \
  --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter \
  --enable-network --disable-autodetect --disable-debug \
  --enable-swscale --enable-swresample \
  --enable-zlib --extra-cflags="-I$ZLIB_INC_W" --extra-ldflags="-LIBPATH:$ZLIB_LIB_W" \
  --pkg-config="$PKGCONF_SHIM" --enable-libxml2 --enable-libsrt \
  --enable-protocol=file,pipe,tcp,udp,rtp,http,https,tls,rtmp,rtmps,hls,crypto,data,libsrt \
  --enable-demuxer="$FF_DEMUXERS" \
  --enable-decoder="$FF_DECODERS" \
  --enable-parser="$FF_PARSERS" \
  --enable-bsf="$FF_BSFS" \
  --enable-hwaccel=h264_d3d11va,h264_d3d11va2,h264_dxva2,hevc_d3d11va,hevc_d3d11va2,hevc_dxva2,av1_d3d11va,av1_d3d11va2,av1_dxva2,vp9_d3d11va,vp9_d3d11va2,vp9_dxva2 \
  --enable-d3d11va --enable-dxva2 \
  --enable-schannel

# configure silently drops a feature whose dependency is missing (schannel is an
# explicit enable under --disable-autodetect and soft-disables when its headers or
# libs are absent; exr and png need zlib; dash needs libxml2; srt needs libsrt; a misspelt --enable-demuxer name matches
# nothing and says nothing): fail instead of shipping a player whose https:// is dead
# or whose EXR / DPX review formats (Media Player Pro) do not open.
for want in CONFIG_NETWORK CONFIG_SCHANNEL CONFIG_TLS_PROTOCOL CONFIG_HTTPS_PROTOCOL \
            CONFIG_RTSP_DEMUXER CONFIG_HLS_DEMUXER CONFIG_UDP_PROTOCOL CONFIG_RTMP_PROTOCOL \
            CONFIG_ZLIB CONFIG_EXR_DECODER CONFIG_PNG_DECODER CONFIG_DPX_DECODER \
            CONFIG_PRORES_DECODER CONFIG_DNXHD_DECODER CONFIG_MXF_DEMUXER \
            CONFIG_IMAGE_EXR_PIPE_DEMUXER CONFIG_IMAGE_DPX_PIPE_DEMUXER \
            CONFIG_LIBXML2 CONFIG_DASH_DEMUXER CONFIG_LIBSRT CONFIG_LIBSRT_PROTOCOL \
            CONFIG_VC1_DECODER CONFIG_JPEG2000_DECODER CONFIG_MPJPEG_DEMUXER CONFIG_WEBVTT_DEMUXER; do
    # (component switches live in config_components.h on FFmpeg >= 6). grep the files
    # directly: `cat ... | grep -q` under pipefail fails when grep -q exits on an early
    # match and cat dies of SIGPIPE (CONFIG_LIBXML2 sits near the top of config.h).
    grep -q "^#define $want 1" config.h config_components.h 2>/dev/null || { echo "ERROR: FFmpeg configure disabled $want" >&2; exit 1; }
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
