// SPDX-License-Identifier: Apache-2.0
//
// EyeAssign (#80) — which half of a stereo source each display view samples.
//
// The draw loop maps the 2-view source onto the N display views by comparing each view's
// eye X against the centre of all of them: left of centre samples the LEFT half.
//
// When the eyes collapse to one point, EVERY view samples the LEFT half, on purpose. In
// MANAGED eye tracking the vendor collapses the eyes on tracking loss as its "go flat"
// signal (runtime docs/specs/vendor/eye-tracking-modes.md). A display that then shows view
// 0 alone looks the same either way, but one that keeps weaving for that single eye point
// (Leia "Show left view when nobody is tracked" = off) would interlace two different halves
// into a ghost; the same half in every view gives it a clean 2D image. Mapping the
// collapsed case by view order was tried for #80 and rejected for that reason.

#pragma once

#include <cstdint>

namespace mp::eyes {

// Eye X spreads at or below this (metres) count as one point. A real eye pair is ~6 cm
// apart; a collapsed one is exactly 0.
constexpr float kCollapsedSpreadM = 1e-5f;

// True when view `v` of `n` (eye X positions `xs`) should sample the LEFT source half,
// before any user / container eye swap is applied.
inline bool IsLeftView(const float* xs, uint32_t n, uint32_t v)
{
    float minX = xs[0];
    float maxX = xs[0];
    for (uint32_t i = 1; i < n; ++i) {
        if (xs[i] < minX) minX = xs[i];
        if (xs[i] > maxX) maxX = xs[i];
    }
    if (maxX - minX <= kCollapsedSpreadM) {
        return true;  // collapsed = the vendor's "go flat": same (left) half everywhere
    }
    return xs[v] <= 0.5f * (minX + maxX);
}

}  // namespace mp::eyes
