// SPDX-License-Identifier: Apache-2.0
//
// VideoStereoProbe — work out how a video file is packed, BEFORE playback opens it (#45).
//
// Deliberately separate from VideoDecoder: the playback decoder owns a state machine
// (hwaccel, frame ring, A/V clock, seeking) that a one-shot probe has no business
// touching, and the probe wants the opposite configuration anyway — a plain software
// decoder with every quality knob turned down.
//
// WHY SYNCHRONOUS, up front, rather than analysing frames as they are presented:
//
//   * A late layout switch changes contentAspect_ and therefore the view rect, so the
//     picture visibly jumps and resizes a second into playback.
//   * Worse, a mono->SBS flip means the opening second of a stereo clip was rendered with
//     the full double-width frame sent to BOTH eyes — precisely the ghosting this feature
//     exists to remove.
//   * And it cannot work at all on the Windows zero-copy path: with MEDIAPLAYER_ZEROCOPY=1
//     the decoded frame lives only in a shared D3D11 texture and FrameRing::Frame::plane[]
//     is empty, so there are no CPU planes to analyse. Do not refactor into that trap.
//
// The cost is that the file is opened twice and the load blocks for up to `budgetMs`.
// That is invisible during arrow-key and slideshow navigation, which already dip to black
// for longer than the budget, and shows up only as a one-off hitch on a cold open.
//
// Measured (Apple silicon, Release, software decode):
//   3840x1080 SBS h264   content path   51 ms, 7 votes from 7 frames
//   3840x1080 fade-in    content path   55 ms, 7 votes from 11 frames (4 dark, skipped)
//   3840x1080 tagged     metadata path  17 ms, stops after 1 decoded frame
//   7680x2160 SBS h264   content path  189 ms, 7 votes from 7 frames
// 8K is the case to watch: it is inside the 250 ms budget but not by much, so on slower
// hardware the budget will cut the vote short. That degrades gracefully — the vote still
// decides at kVoteMin samples — but it is the reason the budget exists.
//
// Header stays libav-free (same discipline as VideoDecoder's pimpl) so nothing else in
// the build needs the ffmpeg include path.
#pragma once

#include "StereoTypes.h"

#include <string>

namespace mp {

struct VideoStereoProbe {
    struct Result {
        bool ok = false;             // the file opened and had a video stream
        int width = 0, height = 0;

        bool haveMeta = false;       // the stream carried AVStereo3D side data
        StereoLayout metaLayout = StereoLayout::Mono;
        bool metaInvert = false;     // AV_STEREO3D_FLAG_INVERT: the packing is R|L

        StereoDetectResult content;  // voted; decided=false if not run or inconclusive
        int framesExamined = 0;
        int votes = 0;
        double elapsedMs = 0.0;
    };

    // `wantContent=false` stops after the first decoded frame's metadata check — used
    // when the filename already decided the layout and we only want the eye-swap hint.
    static Result Run(const std::string& path, bool wantContent, int budgetMs = 250);
};

} // namespace mp
