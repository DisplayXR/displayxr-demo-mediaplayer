// SPDX-License-Identifier: Apache-2.0
//
// Letterbox: the active picture of one RGBA8 image, without its black bars (a letterbox
// above and below, a pillarbox left and right). Pure; tests/letterbox_test.cpp.
//
// The measurement is the DisplayXR runtime's 2D-to-3D letterbox crop
// (src/xrt/auxiliary/util/u_lift_mailbox.c): per row and per column, the fraction of 96
// points across the line whose Rec. 709 luma is above 0.07, and a bar is the run of lines
// from an edge that are "truly black" (under 3% of those points lit). The runtime watches
// a video for 45 frames before it believes a bar; one picture cannot, so these rules
// stand in for that, and each one falls back to no crop on that axis:
//   - bars come in pairs: both ends, about the same size (a film is centred). A lone dark
//     band is a night sky, a shadow or a status bar.
//   - each bar ends at a hard edge: a line at least half lit within a few lines of it. A
//     dark scene fades out; a bar stops.
//   - a bar is at least 2% of the axis, and the picture keeps at least 30% of it.
// StereoDetect's own bar scan (media/StereoDetect.cpp) is looser on purpose: it only
// steers a correlation, so it may crop a dark scene. A crop the user sees may not.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mp {

struct ActiveArea {
    int x = 0, y = 0, w = 0, h = 0;
};

// The active area of the w x h RGBA8 image at `rgba`, `rowStride` bytes per row (a
// sub-rectangle of a larger image works too). True when bars were found: `out` is then the
// picture inside them. False when there are none or the image is not plausibly
// letterboxed: `out` is the whole image.
bool FindActiveArea(const uint8_t* rgba, int w, int h, ptrdiff_t rowStride, ActiveArea& out);

} // namespace mp
