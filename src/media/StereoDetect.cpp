// SPDX-License-Identifier: Apache-2.0
#include "StereoDetect.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace mp {

namespace {

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // half-open
    int W() const { return x1 - x0; }
    int H() const { return y1 - y0; }
};

// A downsampled greyscale grid: gw x gh cells of 0..255-domain floats.
struct Grid {
    std::vector<float> v;
    int w = 0, h = 0;
    float At(int x, int y) const { return v[(size_t)y * w + x]; }
};

struct Stats {
    float mean = 0.0f, sigma = 0.0f, grad = 0.0f;
};

// --- letterbox / pillarbox --------------------------------------------------------
//
// Matching black bars correlate PERFECTLY, so a letterboxed mono frame would otherwise
// look like a flawless stereo match. Crop them off before measuring anything.
//
// The scan runs PER HALF rather than on the whole frame, and the two results are
// intersected. That one choice covers both cases at once: letterboxing puts identical
// bars on the top/bottom of both halves (the intersection keeps the active rows), while
// pillarboxed SBS puts bars at the frame edges AND on either side of the split -- which,
// in half-relative coordinates, is just "a bar at each end of each half". A single
// whole-frame pass would strip the outer pair and leave the inner pair sitting right at
// the correlation's centre.
//
// A row/column counts as a bar when almost none of its sampled pixels rise above
// `barLevel` -- a proportion, not a mean, so a subtitle or a logo inside the bar does not
// defeat it while a genuinely dark scene does.
template <class Sampler>
Rect ScanBars(Sampler at, const Rect& in, const StereoDetectParams& p) {
    const int sx = std::max(1, in.W() / 512);
    const int sy = std::max(1, in.H() / 512);

    auto rowIsBar = [&](int y) {
        int lit = 0, total = 0;
        for (int x = in.x0; x < in.x1; x += sx) { ++total; if (at(x, y) > (float)p.barLevel) ++lit; }
        return total > 0 && (float)lit < p.barFrac * (float)total;
    };
    auto colIsBar = [&](int x) {
        int lit = 0, total = 0;
        for (int y = in.y0; y < in.y1; y += sy) { ++total; if (at(x, y) > (float)p.barLevel) ++lit; }
        return total > 0 && (float)lit < p.barFrac * (float)total;
    };

    Rect r = in;
    while (r.y0 < r.y1 && rowIsBar(r.y0)) ++r.y0;
    while (r.y1 > r.y0 && rowIsBar(r.y1 - 1)) --r.y1;
    while (r.x0 < r.x1 && colIsBar(r.x0)) ++r.x0;
    while (r.x1 > r.x0 && colIsBar(r.x1 - 1)) --r.x1;

    // Never crop more than maxCropFrac off an axis. Without this clamp a night scene or
    // a black-background product shot collapses to a sliver and then abstains.
    if (r.H() < (int)((1.0f - p.maxCropFrac) * (float)in.H())) { r.y0 = in.y0; r.y1 = in.y1; }
    if (r.W() < (int)((1.0f - p.maxCropFrac) * (float)in.W())) { r.x0 = in.x0; r.x1 = in.x1; }
    return r;
}

// Box-average `src` over `r` into a gw x gh grid. A true area average, so both halves are
// sampled identically and any aliasing is coherent across the pair.
template <class Sampler>
Grid BuildGrid(Sampler at, const Rect& r, int gw, int gh) {
    Grid g;
    g.w = gw;
    g.h = gh;
    g.v.resize((size_t)gw * gh);
    for (int cy = 0; cy < gh; ++cy) {
        const int y0 = r.y0 + (int)((int64_t)cy * r.H() / gh);
        const int y1 = std::max(y0 + 1, r.y0 + (int)((int64_t)(cy + 1) * r.H() / gh));
        for (int cx = 0; cx < gw; ++cx) {
            const int x0 = r.x0 + (int)((int64_t)cx * r.W() / gw);
            const int x1 = std::max(x0 + 1, r.x0 + (int)((int64_t)(cx + 1) * r.W() / gw));
            double acc = 0.0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) acc += at(x, y);
            g.v[(size_t)cy * gw + cx] = (float)(acc / ((double)(x1 - x0) * (double)(y1 - y0)));
        }
    }
    return g;
}

Stats GridStats(const Grid& g) {
    Stats s;
    const size_t n = g.v.size();
    if (n == 0) return s;
    double sum = 0.0, sumSq = 0.0;
    for (float f : g.v) { sum += f; sumSq += (double)f * f; }
    s.mean = (float)(sum / (double)n);
    const double var = sumSq / (double)n - (double)s.mean * s.mean;
    s.sigma = (float)std::sqrt(std::max(0.0, var));

    double gradAcc = 0.0;
    int gradN = 0;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x + 1 < g.w; ++x) {
            gradAcc += std::fabs(g.At(x + 1, y) - g.At(x, y));
            ++gradN;
        }
    s.grad = gradN > 0 ? (float)(gradAcc / gradN) : 0.0f;
    return s;
}

// Zero-mean normalised cross-correlation of the two grids at horizontal shift `d`.
// Means are recomputed over the OVERLAP so the shift itself cannot bias the score.
float Ncc(const Grid& L, const Grid& R, int d) {
    const int x0 = std::max(0, -d);
    const int x1 = std::min(L.w, L.w - d);
    if (x1 - x0 < 2) return 0.0f;
    double sl = 0, sr = 0, sll = 0, srr = 0, slr = 0;
    int64_t n = 0;
    for (int y = 0; y < L.h; ++y)
        for (int x = x0; x < x1; ++x) {
            const double l = L.At(x, y), r = R.At(x + d, y);
            sl += l; sr += r; sll += l * l; srr += r * r; slr += l * r;
            ++n;
        }
    if (n == 0) return 0.0f;
    const double cov = slr - sl * sr / (double)n;
    const double vl = sll - sl * sl / (double)n;
    const double vr = srr - sr * sr / (double)n;
    const double den = std::sqrt(vl * vr);
    if (den < 1e-6) return 0.0f;
    return (float)(cov / den);
}

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The whole analysis, parameterised on how to read a pixel so RGBA needs no temp buffer.
template <class Sampler>
StereoDetectResult AnalyzeImpl(Sampler at, int w, int h, const StereoDetectParams& p) {
    StereoDetectResult res;

    if (w < 32 || h < 16) {
        res.abstained = true;
        res.reason = "too small";
        return res;
    }
    const int mid = w / 2;   // odd width: the centre column is dropped

    // Per-half bar scan, then intersect into one common window used for both halves.
    auto atL = [&](int x, int y) { return at(x, y); };
    auto atR = [&](int x, int y) { return at(x + mid, y); };
    const Rect full{0, 0, mid, h};
    const Rect rl = ScanBars(atL, full, p);
    const Rect rr = ScanBars(atR, full, p);
    Rect act{std::max(rl.x0, rr.x0), std::max(rl.y0, rr.y0),
             std::min(rl.x1, rr.x1), std::min(rl.y1, rr.y1)};
    if (act.W() < 16 || act.H() < 8) act = full;   // degenerate intersection: give up on cropping

    res.cropX0 = act.x0; res.cropY0 = act.y0;
    res.cropX1 = act.x1; res.cropY1 = act.y1;

    const int gw = std::min(p.gridWidth, act.W());
    if (gw < 8) {
        res.abstained = true;
        res.reason = "active area too narrow";
        return res;
    }
    int gh = (int)std::lround((double)gw * act.H() / act.W());
    gh = std::min(std::max(gh, 16), 256);
    gh = std::min(gh, act.H());
    if (gh < 4) {
        res.abstained = true;
        res.reason = "active area too short";
        return res;
    }

    const Grid gl = BuildGrid(atL, act, gw, gh);
    const Grid gr = BuildGrid(atR, act, gw, gh);
    const Stats sl = GridStats(gl), sr = GridStats(gr);
    res.sigmaMin = std::min(sl.sigma, sr.sigma);
    res.gradMin = std::min(sl.grad, sr.grad);

    // Abstain gates. These are what make a fade-from-black opening say NOTHING rather
    // than vote mono for the whole clip.
    if (res.sigmaMin < p.minSigma) {
        res.abstained = true;
        res.reason = "low variance";
        return res;
    }
    if (res.gradMin < p.minGradRatio * res.sigmaMin) {
        // Horizontal shift-correlation is meaningless without horizontal texture. Judged
        // against sigma, not as an absolute — see StereoDetectParams::minGradRatio.
        res.abstained = true;
        res.reason = "no horizontal texture";
        return res;
    }
    // NOTE: there is deliberately NO "the halves have different average brightness" gate.
    // The correlation below is zero-mean per overlap, so a brightness offset cannot bias
    // it — and most MONO frames have halves of differing brightness, so such a gate turns
    // a legitimate mono verdict into an abstention, which then falls through to the very
    // aspect rule this layer exists to overrule. (Measured: it abstained on a plain 16:9
    // mono frame.)

    // Peak over a small shift search.
    const int search = std::max(1, (int)std::lround(p.searchFrac * (float)gw));
    float peak = -2.0f;
    int bestD = 0;
    for (int d = -search; d <= search; ++d) {
        const float v = Ncc(gl, gr, d);
        if (v > peak) { peak = v; bestD = d; }
    }

    // Baseline: how well the halves match at shifts NO stereo pair could have. This is
    // the defence against repetitive content -- brick, fences, railings, window bays, a
    // two-panel collage -- which scores high at many shifts, so its baseline rises with
    // its peak and the margin collapses.
    //
    // The far region is sampled DENSELY and reduced to its SECOND-LARGEST value, not to a
    // mean over a handful of fixed probes. Two reasons:
    //
    //   * A sparse probe set is easy to slip past -- a pattern whose period misses those
    //     few offsets looks distinctive when it is not. Measured: with six fixed probes a
    //     period-160px bar pattern kept a 0.81 margin and was wrongly called stereo, and
    //     even a 90th percentile over dense probes left it at 0.47, because the periodic
    //     hits are only ~10% of the samples and sit right at that cut.
    //   * Second-largest rather than max so one spurious spike cannot veto a real stereo
    //     pair. Periodicity never produces just one high far-shift -- it produces one per
    //     period in range -- so nothing is lost by dropping the single best.
    //
    // The question this answers is "does some IMPLAUSIBLE shift explain the halves about
    // as well as the best plausible one?" If yes, the peak means nothing.
    std::vector<float> far;
    const int lo = std::max(2, (int)std::lround(0.20f * (float)gw));
    const int hi = std::max(lo + 1, (int)std::lround(0.48f * (float)gw));
    for (int d = lo; d <= hi && d < gw - 1; ++d) {
        far.push_back(Ncc(gl, gr, d));
        far.push_back(Ncc(gl, gr, -d));
    }
    float baseline = 0.0f;
    if (far.size() >= 2) {
        std::nth_element(far.begin(), far.begin() + 1, far.end(), std::greater<float>());
        baseline = far[1];
    } else if (far.size() == 1) {
        baseline = far[0];
    }

    res.peak = peak;
    res.baseline = baseline;
    res.margin = peak - baseline;
    res.disparityPx = bestD;

    const int maxD = std::max(1, (int)std::lround(p.maxDisparityFrac * (float)gw));
    const bool sharp = res.margin >= p.marginMin;
    const bool strong = peak >= p.peakMin;
    const bool close = std::abs(bestD) <= maxD;

    if (strong && sharp && close) {
        // Full-vs-half uses the ACTIVE frame aspect: two halves wide, cropped height.
        bool ambiguous = false;
        res.layout = StereoDetect::ChooseFullOrHalf(
            (float)(2 * act.W()) / (float)act.H(), ambiguous);
        res.decided = true;
        res.confidence = Clamp01(0.5f * (peak - p.peakMin) / std::max(0.01f, 1.0f - p.peakMin) +
                                 0.5f * std::min(1.0f, res.margin / 0.25f));
        res.reason = ambiguous ? "stereo (full/half ambiguous)" : "stereo";
    } else if (peak < p.monoPeakMax || res.margin < p.monoMarginMax) {
        // A CONFIDENT mono is a real verdict, not a fall-through: it has to outrank the
        // >=1.9 aspect rule, or a 2:1 mono panorama is still split down the middle.
        res.layout = StereoLayout::Mono;
        res.decided = true;
        res.confidence = Clamp01(1.0f - peak / std::max(0.01f, p.monoPeakMax));
        res.reason = "unrelated halves";
    } else {
        // The grey band: something correlates, but not well enough, or not at a
        // plausible disparity. Say nothing and let the next layer down decide.
        res.decided = false;
        res.reason = close ? "inconclusive" : "peak at implausible disparity";
    }
    return res;
}

} // namespace

StereoDetectResult StereoDetect::AnalyzeLuma(const uint8_t* y, int w, int h,
                                             ptrdiff_t rowStride, int pixelStride,
                                             const StereoDetectParams& p) {
    if (!y || w <= 0 || h <= 0 || pixelStride <= 0) {
        StereoDetectResult r;
        r.abstained = true;
        r.reason = "no data";
        return r;
    }
    auto at = [y, rowStride, pixelStride](int x, int py) {
        return (float)y[(ptrdiff_t)py * rowStride + (ptrdiff_t)x * pixelStride];
    };
    return AnalyzeImpl(at, w, h, p);
}

StereoDetectResult StereoDetect::AnalyzeRGBA(const uint8_t* rgba, int w, int h,
                                             ptrdiff_t rowStride,
                                             const StereoDetectParams& p) {
    if (!rgba || w <= 0 || h <= 0) {
        StereoDetectResult r;
        r.abstained = true;
        r.reason = "no data";
        return r;
    }
    // Luma on the fly rather than into a temp plane — an 8K frame would be a 33 MB
    // allocation for a value we read exactly once.
    auto at = [rgba, rowStride](int x, int y) {
        const uint8_t* q = rgba + (ptrdiff_t)y * rowStride + (ptrdiff_t)x * 4;
        return 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
    };
    return AnalyzeImpl(at, w, h, p);
}

StereoLayout StereoDetect::ChooseFullOrHalf(float activeAspect, bool& ambiguous) {
    ambiguous = false;
    if (!(activeAspect > 0.0f)) return StereoLayout::SbsFull;

    // Hard priors first: nothing sane is a half-SBS at 3:1, or a full-SBS at 4:3.
    if (activeAspect >= 3.0f) return StereoLayout::SbsFull;
    if (activeAspect <= 1.4f) return StereoLayout::SbsHalf;

    // Plausible single-eye aspects, incl. the two common cinema ratios.
    static const float kMono[] = {1.0f, 1.25f, 1.3333f, 1.5f, 1.6f,
                                  1.7778f, 1.85f, 2.0f, 2.3333f, 2.39f};
    auto cost = [](float x) {
        float best = 1e30f;
        for (float m : kMono) best = std::min(best, std::fabs(std::log(x) - std::log(m)));
        return best;
    };
    // Full SBS: the frame is two full-width eyes, so per-eye aspect is HALF the frame's.
    // Half SBS: each eye is squeezed and stretched back, so per-eye display aspect EQUALS
    // the frame's.
    const float costFull = cost(activeAspect * 0.5f);
    const float costHalf = cost(activeAspect);
    if (std::fabs(costFull - costHalf) < 0.03f) {
        ambiguous = true;
        return StereoLayout::SbsFull;   // a 1:1-per-eye full SBS is likelier than a 2:1 half
    }
    return costFull <= costHalf ? StereoLayout::SbsFull : StereoLayout::SbsHalf;
}

void StereoVote::Add(const StereoDetectResult& r) {
    if (r.abstained || !r.decided) return;   // silence is not a vote
    const int i = (int)r.layout;
    if (i < 0 || i > 2) return;
    ++count_[i];
    conf_[i] += r.confidence;
    ++samples_;
    if (r.confidence > best_.confidence || samples_ == 1) best_ = r;
}

StereoDetectResult StereoVote::Result() const {
    StereoDetectResult out = best_;
    if (samples_ == 0) {
        out = StereoDetectResult{};
        out.decided = false;
        out.reason = "no usable frames";
        return out;
    }
    int win = 0;
    for (int i = 1; i < 3; ++i) {
        if (count_[i] > count_[win]) {
            win = i;
        } else if (count_[i] == count_[win] && count_[i] > 0) {
            // Tie on count: prefer the layout its samples were more confident about.
            const float mi = conf_[i] / (float)count_[i];
            const float mw = conf_[win] / (float)count_[win];
            if (mi > mw) win = i;
        }
    }
    out.layout = (StereoLayout)win;
    out.confidence = count_[win] > 0 ? conf_[win] / (float)count_[win] : 0.0f;
    out.decided = Sufficient() || acceptSingle_;
    if (!out.decided) out.reason = "too few usable frames";
    return out;
}

void StereoVote::Reset() {
    samples_ = 0;
    for (int i = 0; i < 3; ++i) { count_[i] = 0; conf_[i] = 0.0f; }
    best_ = StereoDetectResult{};
    acceptSingle_ = false;
}

} // namespace mp
