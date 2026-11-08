# SPDX-License-Identifier: Apache-2.0
#
# The slim FFmpeg's demuxers / decoders / parsers / bitstream filters: ONE list for
# Windows (build-ffmpeg-slim.sh), Linux (build_ffmpeg_linux.sh) and macOS
# (build_ffmpeg_macos.sh), sourced by all three so a file that opens on one platform
# opens on the others. Protocols, TLS and hwaccels stay per platform (they follow
# the OS: SChannel / Secure Transport / none; D3D11VA / VAAPI+NVDEC / VideoToolbox).
# Every CI cache key hashes this file.
#
# What the player opens (src/media/MediaSource.cpp): MP4 / MOV / M4V / MKV / WEBM /
# 3GP, stereo HEIC (mov demuxer), live streams (StreamUrl.cpp), and in Media Player
# Pro MXF plus EXR / DPX / TIFF sequences (image2). Decoders cover what those
# containers realistically carry; #201 audited this against the Homebrew FFmpeg the
# macOS build used before (the PR lists the diff and what was left out on purpose).
#
# Notes on individual entries:
#   libdav1d   only takes effect where the build script enables libdav1d (macOS and
#              Linux, #201; the Windows build relies on D3D11VA AV1 for now):
#              FFmpeg's native av1 decoder is hardware-only. FFmpeg prefers libdav1d
#              over the native decoder when both are built.
#   mpjpeg     MJPEG-over-HTTP (multipart), the stream most IP cameras serve.
#   webvtt     HLS subtitle renditions: without it the hls demuxer fails the WHOLE
#              open on a master playlist that lists WebVTT subtitles (#201).
#   dolby_e, s302m, adpcm_g722/g726, pcm_alaw/mulaw: broadcast MXF / TS and RTSP
#              camera audio.

# shellcheck disable=SC2034  # read by the sourcing build scripts
FF_DEMUXERS=mov,matroska,mpegts,avi,flv,live_flv,rtsp,rtp,sdp,hls,dash,wav,flac,ogg,mp3,aac,ac3,eac3,mjpeg,image2,image2pipe,h264,hevc,av1,m4v,mxf,image_dpx_pipe,image_exr_pipe,mpjpeg,webvtt

FF_DECODERS=\
h264,hevc,av1,libdav1d,vp9,vp8,mpeg4,mpeg2video,mpeg1video,h263,vc1,vvc,apv,\
prores,prores_raw,dnxhd,cfhd,ffv1,jpeg2000,dvvideo,qtrle,rawvideo,v210,v210x,r210,r10k,\
mjpeg,png,bmp,tiff,webp,gif,exr,dpx,\
aac,aac_latm,mp1,mp2,mp3,ac3,eac3,dca,truehd,mlp,flac,alac,opus,vorbis,amrnb,amrwb,dolby_e,s302m,\
pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le,pcm_u8,pcm_s8,\
pcm_s16be,pcm_s24be,pcm_s32be,pcm_f32be,pcm_f64be,pcm_alaw,pcm_mulaw,adpcm_g722,adpcm_g726

FF_PARSERS=h264,hevc,av1,vp9,vp8,mpeg4video,mpegvideo,h263,vc1,vvc,apv,prores_raw,dnxhd,jpeg2000,mjpeg,png,aac,aac_latm,mpegaudio,ac3,dca,mlp,flac,opus,vorbis,amr,dolby_e

FF_BSFS=h264_mp4toannexb,hevc_mp4toannexb,av1_frame_split,vp9_superframe,vp9_superframe_split,aac_adtstoasc,extract_extradata,mpeg4_unpack_bframes,null

# Upstream FFmpeg fixes on top of the pinned FFMPEG_SHA: scripts/ffmpeg-patches/*.patch,
# verbatim upstream commits that landed after the pin, applied in name order (a patch
# already applied is skipped, so a reused source tree is fine). Drop each one when the
# pin moves past it. Every CI cache key hashes the folder.
#   0001 54f0296dfe avformat/hls: deduplicate playlists shared by multiple variants.
#        A master that pairs every video variant with several audio groups (Apple's
#        fMP4 reference stream: 8 video playlists x 3 audio groups) opened each video
#        playlist 3 times: 28 streams instead of 12 and a 13-20 s open, at or past the
#        player's 15 s HLS connect budget (#201).
#   0002 055cc2024d avformat/hls: fix variant_bitrate on streams shared by multiple
#        variants. Follow-up to 0001: a shared stream's variant_bitrate would be the
#        last variant's, and the player picks the HLS representation by that tag.
FF_PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ffmpeg-patches"
ff_apply_patches() {   # <ffmpeg-src-dir>
    local p
    for p in "$FF_PATCH_DIR"/*.patch; do
        [ -e "$p" ] || continue
        if (cd "$1" && git apply --reverse --check "$p" 2>/dev/null); then
            echo "ffmpeg patch already applied: $(basename "$p")"
        else
            # A CRLF checkout (git core.autocrlf on a Windows box) would not match the
            # LF patch: give the files it touches LF endings first (MSVC takes either).
            (cd "$1" && sed -n 's|^diff --git a/\([^ ]*\) .*|\1|p' "$p" | while IFS= read -r f; do
                [ -f "$f" ] && perl -pi -e 's/\r$//' "$f"; done)
            (cd "$1" && git apply "$p") || { echo "error: ffmpeg patch does not apply: $(basename "$p")" >&2; return 1; }
            echo "ffmpeg patch applied: $(basename "$p")"
        fi
    done
}
