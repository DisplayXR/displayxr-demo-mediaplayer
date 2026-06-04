// SPDX-License-Identifier: BSL-1.0
//
// FrameRing (STUB — M2). Triple-buffered, lock-light handoff of decoded frames
// from the decode thread to the render thread, decoupling decode rate from
// display rate. Empty for the M0 skeleton.
#pragma once

namespace mp {

class FrameRing {
    // TODO(M2): triple-buffered decoded-frame handoff.
};

} // namespace mp
