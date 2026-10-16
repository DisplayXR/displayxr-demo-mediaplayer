// SPDX-License-Identifier: Apache-2.0
//
// pro::MakeThumbnail — a small RGBA picture of one live frame's left eye, for the
// station's input list (the plan mockup's feed thumbnails). CPU, box-filtered, BT.709;
// called about twice a second per input, so cost is a few hundred microseconds at 4K.
// tests/thumbnail_test.cpp.
#pragma once

#include "media/FrameRing.h"
#include "media/StereoTypes.h"

#include <cstdint>
#include <vector>

namespace mp::pro {

// Fit the frame's left eye (the whole frame when `layout` is Mono) into `outW` x `outH`,
// keeping its display aspect (a half-SBS eye is shown unsqueezed) with black bars.
// `rgba` gets outW * outH * 4 bytes, alpha 255. False for frames with no CPU pixels
// (the zero-copy GPU path) or a format it does not know.
bool MakeThumbnail(const FrameRing::Frame& f, StereoLayout layout, int outW, int outH, std::vector<uint8_t>& rgba);

}  // namespace mp::pro
