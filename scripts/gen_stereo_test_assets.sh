#!/usr/bin/env bash
#
# scripts/gen_stereo_test_assets.sh — generate the stereo-layout detector's test corpus.
#
# The repo ships only `*_2x1`-suffixed assets, so the UNSUFFIXED cases the detector exists
# for had no fixtures at all. This builds them reproducibly.
#
# Output goes to assets/media/detect/ — `assets/media/` is already in .gitignore (as are
# *.mp4 and *.mkv), so nothing here is committed. That also means a fresh clone has none
# of the three `assets/test_*_2x1.mp4` clips either; --legacy regenerates those too.
#
# Usage:
#   ./scripts/gen_stereo_test_assets.sh              detector corpus
#   ./scripts/gen_stereo_test_assets.sh --legacy     ...plus the assets/test_*_2x1.* set
#
# Requires: ffmpeg + ffprobe with libx264 (Homebrew's has it; the slim Windows CI build
# does NOT — this is a dev-machine script). python3 is used only for the MPO fixture.
#
# Genuine stereo pairs are made by cropping ONE wide source at two horizontal offsets.
# That gives a real, uniform disparity — which is what the detector should accept — rather
# than two unrelated images that merely sit side by side.
#
# The source plate is `mandelbrot`, NOT `testsrc2`. testsrc2 is flat colour blocks and
# repeating structure, i.e. highly self-similar under horizontal shift: measured, its two
# halves correlate at 0.97 even at implausible shifts, so a genuine stereo pair has no
# distinctive peak and the detector correctly refuses it. That makes testsrc2 a fixture
# that tests nothing. mandelbrot is aperiodic with real detail at many scales, which is
# what natural photographic content looks like to a correlator.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

LEGACY=false
for arg in "${@}"; do
    case "$arg" in
        --legacy) LEGACY=true ;;
        -h|--help) sed -n '1,22p' "$0"; exit 0 ;;
        *) echo "Unknown arg: $arg" >&2; exit 2 ;;
    esac
done

command -v ffmpeg  >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }
command -v ffprobe >/dev/null || { echo "ffprobe not found" >&2; exit 1; }

O="assets/media/detect"
mkdir -p "$O"
BASE="$(mktemp -d)/base.png"
trap 'rm -rf "$(dirname "$BASE")"' EXIT

echo "==> base plate"
ffmpeg -y -v error -f lavfi -i "mandelbrot=size=4096x1080" -frames:v 1 "$BASE"

# --- stills -------------------------------------------------------------------------
echo "==> stills"

# 1. Unsuffixed FULL SBS: 3840x1080, per-eye 16:9, 24 px disparity.
ffmpeg -y -v error -i "$BASE" -filter_complex \
 "[0:v]crop=1920:1080:0:0[l];[0:v]crop=1920:1080:24:0[r];[l][r]hstack=inputs=2" \
 "$O/sbs_full_unsuffixed.png"

# 2. Unsuffixed HALF SBS: 1920x1080 — pixel-for-pixel indistinguishable from mono 16:9.
#    This is the case NO aspect threshold can ever get right.
ffmpeg -y -v error -i "$BASE" -filter_complex \
 "[0:v]crop=1920:1080:0:0,scale=960:1080[l];[0:v]crop=1920:1080:24:0,scale=960:1080[r];[l][r]hstack=inputs=2" \
 "$O/sbs_half_unsuffixed.png"

# 3-5. Mono at three aspects. mono_2to1 is the trap: the old >=1.9 rule splits it.
#
# These crop from the MIDDLE of the plate (MONO_X), where the set boundary has detail.
# The SBS fixtures can crop from x=0 because each of their halves is a full 1920-wide
# window, but a mono fixture's halves are only half that wide — and the plate's left edge
# is smooth, low-contrast escape region, which measures at sigma ~7.7 and (rightly) makes
# the detector abstain for want of anything to correlate.
MONO_X=1400
ffmpeg -y -v error -i "$BASE" -vf "crop=1920:1080:$MONO_X:0" "$O/mono_16x9.png"
ffmpeg -y -v error -i "$BASE" -vf "crop=2560:1080:768:0" "$O/mono_21x9.png"
# NOTE: "2to1", not "2x1" — a "2x1" anywhere in the PATH is the SBS naming
# convention and would be claimed by the filename layer before the detector runs.
ffmpeg -y -v error -i "$BASE" -vf "crop=2160:1080:968:0" "$O/mono_2to1.png"

# 6a. Letterboxed mono — matching bars correlate perfectly if they aren't cropped first.
ffmpeg -y -v error -i "$BASE" -vf "crop=1920:810:$MONO_X:0,pad=1920:1080:0:135:black" \
 "$O/mono_letterbox_16x9.png"
# 6b. Pillarboxed mono on a 2:1 canvas: aspect says SBS AND the outer bars match.
ffmpeg -y -v error -i "$BASE" -vf "crop=1620:1080:1240:0,pad=2160:1080:270:0:black" \
 "$O/mono_pillarbox_2to1.png"
# 6c. Letterboxed FULL SBS — must still be detected THROUGH the bars.
ffmpeg -y -v error -i "$BASE" -filter_complex \
 "[0:v]crop=1920:810:0:0[l];[0:v]crop=1920:810:24:0[r];[l][r]hstack=inputs=2,pad=3840:1080:0:135:black" \
 "$O/sbs_full_letterboxed.png"
# 6d. Pillarboxed FULL SBS: bars at 0, W/2 +/- a, W. Only a per-half crop fixes this.
ffmpeg -y -v error -i "$BASE" -filter_complex \
 "[0:v]crop=1600:1080:160:0,pad=1920:1080:160:0:black[l];\
  [0:v]crop=1600:1080:184:0,pad=1920:1080:160:0:black[r];[l][r]hstack=inputs=2" \
 "$O/sbs_full_pillarbox.png"

# 7. Near-flat / low texture -> the detector must ABSTAIN, not guess.
ffmpeg -y -v error -f lavfi -i "color=c=0x303234:size=3840x1080" -frames:v 1 \
 -vf "noise=alls=2:allf=t" "$O/flat_lowtexture_2to1.png"

# 8. Mono content deliberately NAMED _2x1: the filename layer must still win.
cp "$O/mono_16x9.png" "$O/mono_but_named_2x1.png"

# --- video --------------------------------------------------------------------------
echo "==> video"

# 9. Unsuffixed SBS clip.
ffmpeg -y -v error -f lavfi -i "mandelbrot=size=4096x1080:rate=30" -t 2 -filter_complex \
 "[0:v]crop=1920:1080:0:0[l];[0:v]crop=1920:1080:24:0[r];[l][r]hstack=inputs=2" \
 -c:v libx264 -pix_fmt yuv420p -crf 20 "$O/sbs_full_video.mp4"

# 10. SBS clip fading in from black over 15 frames (0.5 s @ 30 fps). Frame 0 is flat, so
#     a detector that trusted it would call the whole clip mono.
ffmpeg -y -v error -f lavfi -i "mandelbrot=size=4096x1080:rate=30" -t 3 -filter_complex \
 "[0:v]crop=1920:1080:0:0[l];[0:v]crop=1920:1080:24:0[r];[l][r]hstack=inputs=2,fade=t=in:st=0:d=0.5" \
 -c:v libx264 -pix_fmt yuv420p -crf 20 "$O/sbs_fadein_video.mp4"

# 11. Container metadata: Matroska StereoMode -> AV_PKT_DATA_STEREO3D on demux.
ffmpeg -y -v error -i "$O/sbs_full_video.mp4" -c copy \
 -metadata:s:v:0 stereo_mode=left_right "$O/sbs_meta_lr.mkv"
# 11b. Eye-swapped variant: should surface as SIDEBYSIDE + AV_STEREO3D_FLAG_INVERT.
ffmpeg -y -v error -i "$O/sbs_full_video.mp4" -c copy \
 -metadata:s:v:0 stereo_mode=right_left "$O/sbs_meta_rl.mkv"

# 12. In-BITSTREAM metadata: H.264 frame_packing_arrangement SEI via x264
#     (frame-packing=3 is side-by-side). Exported by the decoder as frame side data.
ffmpeg -y -v error -i "$O/sbs_full_video.mp4" -c:v libx264 -x264-params frame-packing=3 \
 -pix_fmt yuv420p -crf 20 "$O/sbs_meta_sei.mp4"

# NOTE: MP4 st3d/sv3d is deliberately NOT attempted. The mov DEMUXER parses those boxes,
# but the mov muxer does not write them; an MP4-st3d fixture needs Google's spatialmedia
# injector or MP4Box. There is also no "tagging" bitstream filter — libavfilter's stereo3d
# CONVERTS packings, it does not label them; x264's frame-packing is the practical route.

# --- MPO ------------------------------------------------------------------------------
# ffmpeg cannot write MPO, so assemble one: two JPEGs concatenated, indexed by an APP2
# "MPF" segment. Hand-built here from the same spec text the parser was written from,
# which makes it an independent cross-check rather than a copy of the implementation.
echo "==> MPO"
ffmpeg -y -v error -i "$BASE" -vf "crop=1920:1080:0:0"  -q:v 2 "$O/.mpo_left.jpg"
ffmpeg -y -v error -i "$BASE" -vf "crop=1920:1080:24:0" -q:v 2 "$O/.mpo_right.jpg"
python3 - "$O/.mpo_left.jpg" "$O/.mpo_right.jpg" "$O/stereo_pair.mpo" <<'PY'
import struct, sys
left, right, out = sys.argv[1], sys.argv[2], sys.argv[3]
a = open(left, 'rb').read()
b = open(right, 'rb').read()

# MP Index IFD, little-endian ("II"). Every offset below is relative to the TIFF header,
# i.e. the byte right after "MPF\0" -- NOT to the file.
IFD_OFF, MPENTRY_OFF = 8, 50

def ifd(num_images, entries_len):
    p = b'II' + struct.pack('<HI', 42, IFD_OFF)
    p += struct.pack('<H', 3)                                   # 3 IFD entries
    p += struct.pack('<HHI', 0xB000, 7, 4) + b'0100'            # MPFVersion
    p += struct.pack('<HHI', 0xB001, 4, 1) + struct.pack('<I', num_images)
    p += struct.pack('<HHI', 0xB002, 7, entries_len) + struct.pack('<I', MPENTRY_OFF)
    p += struct.pack('<I', 0)                                   # next IFD
    return p

# Two passes: the segment's size feeds the second image's offset, which lives inside it.
payload_len = len(ifd(2, 32).ljust(MPENTRY_OFF, b'\0')) + 32
app2_len = 2 + 4 + payload_len                    # length field + "MPF\0" + payload
primary_len = 2 + 2 + app2_len + (len(a) - 2)     # SOI + marker + segment + rest of A
TIFF_BASE = 2 + 2 + 2 + 4                         # SOI, FFE2, length, "MPF\0"

entries = struct.pack('<IIIHH', 0x030000, primary_len, 0, 0, 0)            # primary
entries += struct.pack('<IIIHH', 0x020002, len(b),                          # disparity view
                       primary_len - TIFF_BASE, 0, 0)
payload = ifd(2, 32).ljust(MPENTRY_OFF, b'\0') + entries
assert len(payload) == payload_len, (len(payload), payload_len)

blob = b'\xff\xd8' + b'\xff\xe2' + struct.pack('>H', 2 + 4 + len(payload)) + b'MPF\0'
blob += payload + a[2:]
assert len(blob) == primary_len, (len(blob), primary_len)
open(out, 'wb').write(blob + b)
print(f"    {out}: primary={primary_len} second={len(b)} total={primary_len + len(b)}")
PY
rm -f "$O/.mpo_left.jpg" "$O/.mpo_right.jpg"

# --- legacy clips ----------------------------------------------------------------------
if [ "$LEGACY" = true ]; then
    echo "==> legacy assets/test_*_2x1"
    ffmpeg -y -v error -f lavfi -i "mandelbrot=size=4096x1080:rate=30" -t 5 -filter_complex \
     "[0:v]crop=1920:1080:0:0[l];[0:v]crop=1920:1080:24:0[r];[l][r]hstack=inputs=2" \
     -c:v libx264 -pix_fmt yuv420p -crf 20 "assets/test_SBS_2x1.mp4"
    ffmpeg -y -v error -f lavfi -i "mandelbrot=size=4096x1080:rate=30" -t 5 -filter_complex \
     "[0:v]crop=1920:1080:0:0,colorchannelmixer=rr=1:gg=0.4:bb=0.4[l];\
      [0:v]crop=1920:1080:24:0,colorchannelmixer=rr=0.4:gg=0.4:bb=1[r];[l][r]hstack=inputs=2" \
     -c:v libx264 -pix_fmt yuv420p -crf 20 "assets/test_LRtint_2x1.mp4"
    ffmpeg -y -v error -f lavfi -i "mandelbrot=size=4096x1080:rate=30" -t 5 -filter_complex \
     "[0:v]crop=1920:1080:0:0,zoompan=z='min(zoom+0.0015,1.5)':d=150:s=1920x1080[l];\
      [0:v]crop=1920:1080:24:0,zoompan=z='min(zoom+0.0015,1.5)':d=150:s=1920x1080[r];\
      [l][r]hstack=inputs=2" \
     -c:v libx264 -pix_fmt yuv420p -crf 20 "assets/test_zoom_2x1.mp4"
fi

# --- self-check -------------------------------------------------------------------------
# The metadata muxing above is the part of this script worth distrusting: whether the
# matroska muxer honours a `stereo_mode` stream tag with that exact spelling, and whether
# right_left really surfaces as INVERT, are not obvious from the docs. Fail loudly HERE
# rather than shipping a fixture that silently carries no side data.
#
# Check the FRAME level, not the stream level. sbs_meta_sei.mp4 has no stream-level side
# data at all -- its frame-packing arrangement lives in the H.264 bitstream and only
# materialises once the decoder emits a frame. (That is exactly why VideoStereoProbe reads
# AV_FRAME_DATA_STEREO3D and never the stream-level API, which would miss this file.)
# Note ffprobe prints only `side_data_type` per frame; the packing detail and the invert
# bit come from the stream view, which the MKVs do expose.
echo "==> verifying container metadata"
fail=0
check_frame_stereo3d() {
    ffprobe -v error -select_streams v:0 -show_frames -read_intervals '%+#1' \
        -show_entries frame=side_data_list "$1" 2>/dev/null | grep -qi 'Stereo 3D'
}
for f in "$O/sbs_meta_lr.mkv" "$O/sbs_meta_rl.mkv" "$O/sbs_meta_sei.mp4"; do
    if check_frame_stereo3d "$f"; then
        echo "    OK   $(basename "$f") — AVStereo3D on the first decoded frame"
    else
        echo "    FAIL $(basename "$f"): no stereo3d frame side data" >&2
        fail=1
    fi
done
# The MKVs additionally carry it at stream level, so the packing and the eye order can be
# asserted rather than assumed.
for pair in "sbs_meta_lr.mkv:0" "sbs_meta_rl.mkv:1"; do
    f="${pair%%:*}"; want="${pair##*:}"
    sd="$(ffprobe -v error -select_streams v:0 -show_entries stream_side_data_list \
          -of default "$O/$f" 2>/dev/null || true)"
    echo "$sd" | grep -qi 'type=side by side' || {
        echo "    FAIL $f: not tagged side-by-side" >&2; fail=1; }
    echo "$sd" | grep -q "inverted=$want" || {
        echo "    FAIL $f: expected inverted=$want" >&2; fail=1; }
done
if [ "$fail" -ne 0 ]; then
    echo "" >&2
    echo "One or more metadata fixtures are not tagged as expected." >&2
    echo "Fallback for the MKV pair: mkvmerge --stereo-mode 0:left_right" >&2
    exit 1
fi

echo ""
echo "Wrote $(ls -1 "$O" | wc -l | tr -d ' ') asset(s) to $O"
ls -1 "$O"
