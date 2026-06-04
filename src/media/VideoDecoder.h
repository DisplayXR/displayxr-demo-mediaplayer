// SPDX-License-Identifier: BSL-1.0
//
// VideoDecoder (STUB — M2/M3). FFmpeg libav* decode with per-OS hwaccel
// auto-select + software fallback, on a decode thread, emitting frames into a
// FrameRing. Empty for the M0 skeleton.
#pragma once

namespace mp {

class VideoDecoder {
    // TODO(M2): FFmpeg demux/decode thread -> FrameRing.
};

} // namespace mp
