// SPDX-License-Identifier: Apache-2.0
#include "media/Letterbox.h"

#include <algorithm>
#include <cstdlib>

namespace mp {

namespace {

constexpr int kSamples = 96;            // points across one line (the runtime's LINE_SAMPLES)
constexpr int kLumaMin = 178500;        // 0.07 of 255, in the luma sum's units (weights x 10000)
constexpr float kBlackFrac = 0.03f;     // a bar line has fewer of its points lit than this
constexpr float kMinBarFrac = 0.02f;    // the smallest bar, as a fraction of the axis
constexpr int kMinBarPx = 2;
constexpr float kSymmetryFrac = 0.025f; // how far the two bars may differ
constexpr float kEdgeFrac = 0.01f;      // how close to the bar the picture must start
constexpr float kEdgeLitFrac = 0.5f;    // and how lit that first line must be
constexpr float kMinActiveFrac = 0.3f;  // the picture keeps at least this much of the axis

// floor((k + 0.5) * span / n): the runtime's sample positions.
int SamplePos(int k, int n, int span) { return (int)(((int64_t)(2 * k + 1) * span) / (2 * n)); }

bool Lit(const uint8_t* px) { return 2126 * px[0] + 7152 * px[1] + 722 * px[2] > kLumaMin; }

// The bars at the two ends of one axis of `len` lines, each line `span` pixels across.
// `at(line, pos)` is the pixel at position `pos` along line `line`. Zero when the axis has
// no plausible pair.
template <class At>
void Axis(int len, int span, At at, int& lo, int& hi) {
    lo = hi = 0;
    if (len < 8 || span < 1) return;
    const int n = std::min(kSamples, span);
    auto frac = [&](int line) {
        int lit = 0;
        for (int k = 0; k < n; ++k) lit += Lit(at(line, SamplePos(k, n, span))) ? 1 : 0;
        return (float)lit / (float)n;
    };
    int a = 0;
    while (a < len && frac(a) < kBlackFrac) ++a;
    if (a == len) return;  // black throughout: nothing to measure
    int b = 0;
    while (b < len && frac(len - 1 - b) < kBlackFrac) ++b;

    const int minBar = std::max(kMinBarPx, (int)(kMinBarFrac * (float)len + 0.5f));
    if (a < minBar || b < minBar) return;
    if (std::abs(a - b) > std::max(2, (int)(kSymmetryFrac * (float)len + 0.5f))) return;
    if ((float)(len - a - b) < kMinActiveFrac * (float)len) return;
    const int edge = std::max(2, (int)(kEdgeFrac * (float)len + 0.5f));
    auto hard = [&](int first, int step) {
        for (int i = 0; i < edge; ++i) {
            if (frac(first + i * step) >= kEdgeLitFrac) return true;
        }
        return false;
    };
    if (!hard(a, 1) || !hard(len - 1 - b, -1)) return;
    lo = a;
    hi = b;
}

} // namespace

bool FindActiveArea(const uint8_t* rgba, int w, int h, ptrdiff_t rowStride, ActiveArea& out) {
    out = ActiveArea{0, 0, std::max(0, w), std::max(0, h)};
    if (!rgba || w <= 0 || h <= 0) return false;
    auto px = [&](int x, int y) { return rgba + (ptrdiff_t)y * rowStride + (ptrdiff_t)x * 4; };
    int top = 0, bottom = 0, left = 0, right = 0;
    Axis(h, w, [&](int line, int pos) { return px(pos, line); }, top, bottom);
    // Columns are measured inside the letterbox, so its bars do not count as a column's dark.
    const int y0 = top, rows = h - top - bottom;
    Axis(w, rows, [&](int line, int pos) { return px(line, y0 + pos); }, left, right);
    if (top + bottom + left + right == 0) return false;
    out = ActiveArea{left, top, w - left - right, h - top - bottom};
    return true;
}

} // namespace mp
