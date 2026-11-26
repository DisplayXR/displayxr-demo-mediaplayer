// SPDX-License-Identifier: Apache-2.0
//
// DxrStereo — what the player reads from a DisplayXR Camera file (the DisplayXR stereo
// media format, version 1; third_party/dxrmedia/VENDORED.md names the contract).
//
//   * still:  a `.jpg` whose second view is an MPF image, `dxr:Layout="mpf-pair"`
//             (MpoLoader composes the pair), or the side-by-side export, one JPEG,
//             `dxr:Layout="sbs-lr"`.
//   * video:  a full side-by-side HEVC `*_sbs.mp4`: the same XMP packet in a top-level
//             `uuid` box, plus a timed-metadata track (`mett`, MIME
//             application/x-displayxr-stereo) with one 32-byte sample per frame.
//
// The parsing is the format's reference reader (dxrmedia, vendored unmodified), not a
// second parser. This file only turns its answer into the player's terms.
//
// THE ONE SIGN / UNIT CONVERSION (ShiftForDisparity below). The file's convergence is a
// disparity d = x_left - x_right of the scene point the camera suggests for the screen
// plane, as a fraction of ONE eye's width (a parallel rig: d >= 0, 0 at infinity). The
// player's convergence is a per-eye SHIFT s, as a fraction of the eye's width, applied
// +s to the left view and -s to the right (autoconv::ConvergenceShiftPx). A point of
// disparity d is shown with disparity d + 2s, so the suggested plane lands at zero
// parallax for s = -d/2. That matches FORMAT.md 0 ("shift the left view by
// -Convergence/2 and the right view by +Convergence/2"). Every value this file hands
// out is already a shift; nothing downstream sees a file disparity.
//
// Files without `dxr:Version` are not ours: every reader here says so and the caller
// carries on exactly as before. Pure: dxrmedia + the C++ standard library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mp::dxrfile {

// File disparity d (x_left - x_right, fraction of eye width) -> the player's per-eye
// shift. The only place the sign is converted.
inline float ShiftForDisparity(float d) { return -0.5f * d; }

struct StillInfo {
    bool ours = false;            // the XMP carries dxr:Version
    bool pair = false;            // dxr:Layout = mpf-pair (two views in an MPF file)
    bool sbs = false;             // dxr:Layout = sbs-lr (one side-by-side JPEG)
    bool hasConvergence = false;  // dxr:Convergence present (it is optional)
    float shift = 0.0f;           // the suggested convergence as the player's per-eye shift
    int eyeWidth = 0, eyeHeight = 0;
};

// A still held in memory (the whole file, or at least through image 0's header
// segments). Never reads out of bounds; anything malformed is simply "not ours".
StillInfo ReadStill(const uint8_t* data, size_t size);
StillInfo ReadStillFile(const std::string& path);

struct VideoInfo {
    bool ours = false;            // the XMP uuid box carries dxr:Version
    bool sbs = false;             // dxr:Layout = sbs-lr: full side-by-side, left | right
    bool hasConvergence = false;  // static dxr:Convergence (the clip's median)
    float shift = 0.0f;           // ...as the player's per-eye shift
    uint32_t metaTrackId = 0;     // tkhd id of the timed-metadata track, 0 = none
    uint32_t metaSamples = 0;     // its sample count
};

// Reads only the box tree and the XMP box (no media data). The fd is not consumed and
// its file offset is not used; it must be the whole file from byte 0.
VideoInfo ReadVideoFd(int fd);
VideoInfo ReadVideo(const std::string& path);

// The per-frame convergence of a clip (FORMAT.md 3.3), from the metadata track's samples
// with the timestamps the demuxer gave them. Samples whose convergence-valid bit is
// clear are skipped. Between samples the value is interpolated linearly, except across a
// scene cut, where it steps (the camera's own filter snapped there too).
class ConvergenceCurve {
public:
    // One sample at `ptsUs` (the demuxer's presentation time, microseconds). Returns
    // whether it carried a valid convergence. Samples may arrive in any order.
    bool Add(int64_t ptsUs, const uint8_t* sample, size_t size);
    bool Empty() const { return points_.empty(); }
    size_t Size() const { return points_.size(); }
    // The player's per-eye shift at `ptsUs`; `fallback` when the curve is empty. Before
    // the first sample / after the last it holds the first / last value.
    float ShiftAt(int64_t ptsUs, float fallback) const;
    void Clear() { points_.clear(); }

private:
    struct Point {
        int64_t ptsUs;
        float shift;  // already converted (ShiftForDisparity)
        bool cut;     // a scene cut at this frame
    };
    std::vector<Point> points_;  // kept sorted by ptsUs (Add inserts in place)
};

}  // namespace mp::dxrfile
