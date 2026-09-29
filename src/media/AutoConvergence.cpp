// SPDX-License-Identifier: Apache-2.0
// Port of displayxr-web js/call/disparity.js (see AutoConvergence.h for the contract).
#include "media/AutoConvergence.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace mp::autoconv {
namespace {

inline int RoundJs(double v) { return (int)std::floor(v + 0.5); }  // Math.round

// NCC of a bw x bh left-eye block at (lx, ly) against the right eye at (rx, ry).
// -1 when the right block would leave the right eye or is flat.
float Ncc(const SbsGray& img, int lx, int ly, int rx, int ry, int bw, int bh, float lMean,
          float lStd) {
    const int W = img.W, E = img.E();
    if (rx < 0 || rx + bw > E || ry < 0) return -1.f;
    double sum = 0, sum2 = 0, cross = 0;
    const int n = bw * bh;
    for (int y = 0; y < bh; ++y) {
        const uint8_t* lo = img.p + (size_t)(ly + y) * W + lx;
        const uint8_t* ro = img.p + (size_t)(ry + y) * W + E + rx;
        int64_t s = 0, s2 = 0, cr = 0;
        for (int x = 0; x < bw; ++x) {
            const int r = ro[x];
            s += r;
            s2 += r * r;
            cr += lo[x] * r;
        }
        sum += (double)s;
        sum2 += (double)s2;
        cross += (double)cr;
    }
    const double rMean = sum / n;
    const double rVar = sum2 / n - rMean * rMean;
    if (rVar < 1.0) return -1.f;
    return (float)((cross / n - lMean * rMean) / (lStd * std::sqrt(rVar)));
}

void BlockStats(const SbsGray& img, int x0, int y0, int bw, int bh, float& mean, float& stdv) {
    int64_t sum = 0, sum2 = 0;
    for (int y = 0; y < bh; ++y) {
        const uint8_t* o = img.p + (size_t)(y0 + y) * img.W + x0;
        for (int x = 0; x < bw; ++x) {
            sum += o[x];
            sum2 += o[x] * o[x];
        }
    }
    const double n = (double)bw * bh;
    const double m = (double)sum / n;
    mean = (float)m;
    stdv = (float)std::sqrt(std::max(0.0, (double)sum2 / n - m * m));
}

float BlockSharpness(const SbsGray& img, int x0, int y0, int bw, int bh) {
    int64_t acc = 0;
    int n = 0;
    for (int y = 0; y + 1 < bh; ++y) {
        const uint8_t* o = img.p + (size_t)(y0 + y) * img.W + x0;
        const uint8_t* d = o + img.W;
        for (int x = 0; x + 1 < bw; ++x) {
            acc += std::abs((int)o[x + 1] - (int)o[x]) + std::abs((int)d[x] - (int)o[x]);
            ++n;
        }
    }
    return n ? (float)acc / (float)n : 0.f;
}

struct Resolved {
    int b, step, dMin, dMax;
};
Resolved Resolve(const SbsGray& img, const SearchOptions& o) {
    const int E = img.E();
    Resolved r;
    r.b = o.block > 0 ? o.block : std::max(8, RoundJs(E / 20.0));
    r.step = o.step > 0 ? o.step : RoundJs(r.b * 1.5);
    r.dMax = o.dMax != SearchOptions::kAutoRange ? o.dMax : RoundJs(E * 0.3);
    r.dMin = o.dMin != SearchOptions::kAutoRange ? o.dMin : -RoundJs(E * 0.05);
    return r;
}

float WeightedMedian(std::vector<std::pair<float, float>> dv) {  // (d, weight)
    std::sort(dv.begin(), dv.end());
    double total = 0;
    for (auto& p : dv) total += p.second;
    double acc = 0;
    for (auto& p : dv) {
        acc += p.second;
        if (acc >= 0.5 * total) return p.first;
    }
    return dv.empty() ? 0.f : dv.back().first;
}

float Percentile(std::vector<float> v, float q) {
    std::sort(v.begin(), v.end());
    const size_t i = (size_t)std::floor(q * (float)(v.size() - 1) + 0.5f);
    return v[std::min(i, v.size() - 1)];
}

}  // namespace

// ---- matchBlock -----------------------------------------------------------------------
bool MatchBlock(const SbsGray& img, int x0, int y0, int bw, int bh, int dMin, int dMax,
                int dyMax, float uniq, Match& out) {
    const int H = img.H;
    float mean, stdv;
    BlockStats(img, x0, y0, bw, bh, mean, stdv);
    if (stdv < kMinTexture) return false;
    float best = -2.f;
    int bd = 0, bdy = 0;
    const int nD = dMax - dMin + 1;
    if (nD <= 0) return false;
    std::vector<float> perD((size_t)nD, -2.f);  // best NCC at each disparity (any row)
    std::vector<char> seen((size_t)nD, 0);
    auto getD = [&](int d) -> float {
        const int i = d - dMin;
        return (i >= 0 && i < nD && seen[(size_t)i]) ? perD[(size_t)i] : -2.f;
    };
    auto scan = [&](int d0, int d1, int dStep, int y0s, int y1s) {
        for (int dy = y0s; dy <= y1s; ++dy) {
            const int ry = y0 + dy;
            if (ry < 0 || ry + bh > H) continue;
            for (int d = d0; d <= d1; d += dStep) {
                const float c = Ncc(img, x0, y0, x0 - d, ry, bw, bh, mean, stdv);
                const size_t i = (size_t)(d - dMin);
                if (c > getD(d)) { perD[i] = c; seen[i] = 1; }
                if (c > best) { best = c; bd = d; bdy = dy; }
            }
        }
    };
    // Coarse (every 2nd disparity), then full resolution around the winner.
    const bool coarse = dMax - dMin > 8;
    scan(dMin, dMax, coarse ? 2 : 1, -dyMax, dyMax);
    if (coarse && best > -1.f) {
        const int cd = bd, cy = bdy;
        scan(std::max(dMin, cd - 2), std::min(dMax, cd + 2), 1, std::max(-dyMax, cy - 1),
             std::min(dyMax, cy + 1));
    }
    if (best < -1.f) return false;
    // Uniqueness: the best peak must clearly beat every peak >= 3 px away; rival local
    // maxima of the coarse scan are refined at +-1 so an exact period cannot slip through.
    float second = -2.f;
    auto peakAt = [&](int d) {
        float v = -2.f;
        for (int dy = -dyMax; dy <= dyMax; ++dy) {
            const int ry = y0 + dy;
            if (ry < 0 || ry + bh > H) continue;
            for (int k = d - 1; k <= d + 1; ++k)
                v = std::max(v, Ncc(img, x0, y0, x0 - k, ry, bw, bh, mean, stdv));
        }
        return v;
    };
    for (int i = 0; i < nD; ++i) {
        if (!seen[(size_t)i]) continue;
        const int d = dMin + i;
        const float c = perD[(size_t)i];
        if (std::abs(d - bd) < 3 || c <= second) continue;
        const bool isLocalMax = c >= getD(d - 2) && c >= getD(d + 2);
        const float v = (isLocalMax && c > 0.f) ? peakAt(d) : c;
        if (v > second) second = v;
    }
    // Only for WIDE searches (a narrow tracking window cannot alias).
    if (dMax - dMin > 12 && second > -2.f && best - second < uniq) return false;
    // Sub-pixel along d at the best row.
    const float cm = Ncc(img, x0, y0, x0 - (bd - 1), y0 + bdy, bw, bh, mean, stdv);
    const float cp = Ncc(img, x0, y0, x0 - (bd + 1), y0 + bdy, bw, bh, mean, stdv);
    float sub = 0.f;
    const float den = cm - 2.f * best + cp;
    if (cm > -1.f && cp > -1.f && den < 0.f)
        sub = std::max(-0.5f, std::min(0.5f, (0.5f * (cm - cp)) / den));
    out.d = (float)bd + sub;
    out.dy = bdy;
    out.c = best;
    return true;
}

// ---- blockDisparities -------------------------------------------------------------------
std::vector<Block> BlockDisparities(const SbsGray& img, const SearchOptions& o) {
    const int E = img.E(), H = img.H;
    const Resolved r = Resolve(img, o);
    const int x0 = RoundJs(E * o.roiX0);
    const int x1 = RoundJs(E * o.roiX1) - r.b;
    const int y0 = RoundJs(H * o.roiY0);
    const int y1 = RoundJs(H * o.roiY1) - r.b;
    std::vector<Block> out;
    for (int y = y0; y <= y1; y += r.step) {
        for (int x = x0; x <= x1; x += r.step) {
            Match m;
            if (MatchBlock(img, x, y, r.b, r.b, r.dMin, r.dMax, o.dyMax, o.uniq, m) &&
                m.c >= o.minNcc) {
                Block b;
                b.x = x;
                b.y = y;
                b.d = m.d;
                b.dy = m.dy;
                b.c = m.c;
                b.sharp = BlockSharpness(img, x, y, r.b, r.b);
                out.push_back(b);
            }
        }
    }
    return out;
}

// ---- nearestMode / eyeMidpoint ------------------------------------------------------------
bool NearestMode(const std::vector<Block>& blocks, float tol, float minFrac, int minBlocks,
                 float& d, std::vector<Block>* members) {
    if (blocks.empty()) return false;
    const size_t need =
        std::max((size_t)minBlocks, (size_t)std::ceil((double)blocks.size() * minFrac));
    std::vector<float> ds;
    ds.reserve(blocks.size());
    for (const Block& b : blocks) ds.push_back(b.d);
    std::sort(ds.begin(), ds.end(), std::greater<float>());  // nearest first
    for (float cand : ds) {
        std::vector<float> md;
        for (const Block& b : blocks)
            if (std::fabs(b.d - cand) <= tol) md.push_back(b.d);
        if (md.size() >= need) {
            std::sort(md.begin(), md.end());
            d = md[md.size() >> 1];
            if (members) {
                members->clear();
                for (const Block& b : blocks)
                    if (std::fabs(b.d - cand) <= tol) members->push_back(b);
            }
            return true;
        }
    }
    return false;
}

void EyeMidpoint(const std::vector<Block>& members, int block, float& x, float& y) {
    int top = members.front().y;
    for (const Block& m : members) top = std::min(top, m.y);
    int xMin = 1 << 30, xMax = -(1 << 30);
    for (const Block& m : members) {
        if (m.y > top + 2 * block) continue;
        xMin = std::min(xMin, m.x);
        xMax = std::max(xMax, m.x);
    }
    xMax += block;
    const float headW = (float)(xMax - xMin);
    x = (float)(xMin + xMax) * 0.5f;
    y = (float)top + 0.4f * 1.3f * headW;
}

const char* MethodName(Method m) {
    switch (m) {
        case Method::Mode: return "mode";
        case Method::Focus: return "focus";
        case Method::Track: return "track";
        case Method::Sharp: return "sharp";
        case Method::Centre: return "centre";
    }
    return "?";
}

// ---- measureFocusDisparity ----------------------------------------------------------------
Measurement MeasureFocusDisparity(const SbsGray& img, const SearchOptions& o,
                                  const float* focusXY, std::vector<Block>* blocksOut) {
    const int E = img.E(), H = img.H;
    const Resolved r = Resolve(img, o);
    Measurement res;
    float fx = 0.f, fy = 0.f;
    bool haveD0 = false;
    float d0 = 0.f;
    res.method = Method::Focus;
    if (focusXY) {
        fx = focusXY[0];
        fy = focusXY[1];
    } else {
        SearchOptions so = o;
        so.block = r.b;
        so.dMin = r.dMin;
        so.dMax = r.dMax;
        std::vector<Block> all = BlockDisparities(img, so);
        res.blocks = (int)all.size();
        if (blocksOut) *blocksOut = all;
        std::vector<Block> members;
        float md = 0.f;
        if (!NearestMode(all, o.tol, o.minFrac, o.minBlocks, md, &members)) return res;
        EyeMidpoint(members, r.b, fx, fy);
        d0 = md;
        haveD0 = true;
        res.method = Method::Mode;
    }
    // Refine with one wider window centred on the focus point.
    const int ww = RoundJs(r.b * o.refineW);
    const int wh = RoundJs(r.b * o.refineH);
    const int rx = RoundJs(std::max(0.f, std::min((float)(E - ww), fx - ww / 2.f)));
    const int ry = RoundJs(std::max(0.f, std::min((float)(H - wh), fy - wh / 2.f)));
    const int lo = haveD0 ? std::max(r.dMin, (int)std::floor(d0) - 4) : r.dMin;
    const int hi = haveD0 ? std::min(r.dMax, (int)std::ceil(d0) + 4) : r.dMax;
    Match m;
    res.x = fx;
    res.y = fy;
    if (MatchBlock(img, rx, ry, ww, wh, lo, hi, o.dyMax, kUniqueness, m) && m.c >= o.minNcc) {
        res.ok = true;
        res.d = m.d;
        res.c = m.c;
        return res;
    }
    // A flat or occluded focus window: fall back to the subject's mode.
    if (haveD0) {
        res.ok = true;
        res.d = d0;
        res.c = 0.f;
    }
    return res;
}

// ---- createDisparityTrack -----------------------------------------------------------------
bool DisparityTrack::Push(bool ok, float d) {
    if (!ok || !std::isfinite(d)) return has_;
    hist_.push_back(d);
    if ((int)hist_.size() > n_) hist_.erase(hist_.begin());
    std::vector<float> s = hist_;
    std::sort(s.begin(), s.end());
    value_ = s[s.size() >> 1];
    has_ = true;
    return true;
}

// ---- createFocusTracker -------------------------------------------------------------------
namespace {
std::vector<float> CopyPatch(const SbsGray& img, int x, int y, int w, int h) {
    std::vector<float> p((size_t)w * h);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) p[(size_t)r * w + c] = img.p[(size_t)(y + r) * img.W + x + c];
    return p;
}

// Best NCC position of `tpl` (w x h) in the LEFT eye within +-rx/+-ry of (x0, y0).
void FindInLeft(const SbsGray& img, const std::vector<float>& tpl, int w, int h, int x0, int y0,
                int rx, int ry, int& bx, int& by, float& bc) {
    const int E = img.E(), H = img.H, W = img.W;
    const int n = w * h;
    double tm = 0, t2 = 0;
    for (int i = 0; i < n; ++i) { tm += tpl[(size_t)i]; t2 += tpl[(size_t)i] * tpl[(size_t)i]; }
    tm /= n;
    const double ts = std::sqrt(std::max(1e-6, t2 / n - tm * tm));
    double best = -2;
    bx = x0;
    by = y0;
    for (int y = std::max(0, y0 - ry); y <= std::min(H - h, y0 + ry); ++y) {
        for (int x = std::max(0, x0 - rx); x <= std::min(E - w, x0 + rx); ++x) {
            double s = 0, s2 = 0, cr = 0;
            for (int r = 0; r < h; ++r) {
                const uint8_t* o = img.p + (size_t)(y + r) * W + x;
                const float* t = tpl.data() + (size_t)r * w;
                for (int c = 0; c < w; ++c) {
                    const double v = o[c];
                    s += v;
                    s2 += v * v;
                    cr += v * t[c];
                }
            }
            const double m = s / n;
            const double vr = s2 / n - m * m;
            if (vr < 1) continue;
            const double cc = (cr / n - m * tm) / (std::sqrt(vr) * ts);
            if (cc > best) { best = cc; bx = x; by = y; }
        }
    }
    bc = (float)best;
}
}  // namespace

Measurement FocusTracker::Measure(const SbsGray& img, double nowMs, const SearchOptions& o,
                                  const float* focusXY, std::vector<Block>* blocksOut) {
    const int E = img.E(), H = img.H;
    const Resolved r = Resolve(img, o);
    const int w = RoundJs(r.b * 3.0), h = RoundJs(r.b * 1.5);
    if (blocksOut) blocksOut->clear();
    // Track first, whenever there is something to track.
    bool tracked = false;
    Match tm;
    int tfx = 0, tfy = 0;
    if (st_.valid && st_.w == w && st_.h == h) {
        float fc;
        FindInLeft(img, st_.tpl, w, h, st_.x, st_.y, cfg_.rx, cfg_.ry, tfx, tfy, fc);
        if (fc >= cfg_.minTrackNcc) {
            if (MatchBlock(img, tfx, tfy, w, h, (int)std::floor(st_.d) - cfg_.dWin,
                           (int)std::ceil(st_.d) + cfg_.dWin, 2, kUniqueness, tm) &&
                tm.c >= kMinNcc)
                tracked = true;
        }
    }
    auto accept = [&]() {
        st_.x = tfx;
        st_.y = tfy;
        st_.d = tm.d;
        st_.tpl = CopyPatch(img, tfx, tfy, w, h);
        Measurement m;
        m.ok = true;
        m.d = tm.d;
        m.x = tfx + w / 2.f;
        m.y = tfy + h / 2.f;
        m.c = tm.c;
        m.method = Method::Track;
        return m;
    };
    if (tracked && nowMs - st_.at < cfg_.fullEveryMs) return accept();
    Measurement m = MeasureFocusDisparity(img, o, focusXY, blocksOut);
    if (tracked && (!m.ok || std::fabs(m.d - tm.d) > cfg_.jump)) {
        // A re-search that disagrees with a HEALTHY track needs confirmation by the next.
        const bool confirmed = m.ok && pendingValid_ && std::fabs(pendingD_ - m.d) <= 3.f;
        pendingValid_ = m.ok && !confirmed;
        pendingD_ = m.d;
        if (!confirmed) {
            st_.at = nowMs;
            return accept();
        }
    } else {
        pendingValid_ = false;
    }
    if (!m.ok) {
        st_.valid = false;
        return m;
    }
    const int x = RoundJs(std::max(0.f, std::min((float)(E - w), m.x - w / 2.f)));
    const int y = RoundJs(std::max(0.f, std::min((float)(H - h), m.y - h / 2.f)));
    st_.valid = true;
    st_.x = x;
    st_.y = y;
    st_.w = w;
    st_.h = h;
    st_.d = m.d;
    st_.at = nowMs;
    st_.tpl = CopyPatch(img, x, y, w, h);
    return m;
}

// ---- downsampleLuma ------------------------------------------------------------------------
void DownsampleLuma(const uint8_t* src, int stride, int srcW, int srcH, int factor,
                    bool swapEyes, std::vector<uint8_t>& out, int& outW, int& outH) {
    const int f = std::max(1, factor);
    int w = srcW / f;
    w -= w & 1;  // even, so the SBS halves split cleanly
    const int h = srcH / f;
    outW = w;
    outH = h;
    out.resize((size_t)w * h);
    const int half = w / 2;
    // With swapEyes the output's left half samples the source's right half.
    auto dstX = [&](int x) { return swapEyes ? (x < half ? x + half : x - half) : x; };
    if (f <= 2) {
        const int n = f * f;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = out.data() + (size_t)y * w;
            for (int x = 0; x < w; ++x) {
                int sum = 0;
                for (int dy = 0; dy < f; ++dy) {
                    const uint8_t* o = src + (size_t)(y * f + dy) * stride + (size_t)x * f;
                    for (int dx = 0; dx < f; ++dx) sum += o[dx];
                }
                row[dstX(x)] = (uint8_t)(sum / n);
            }
        }
        return;
    }
    // Larger factors: a 2x2 sample at the quarter points of each cell.
    const int a = f >> 2, c = (3 * f) >> 2;
    for (int y = 0; y < h; ++y) {
        const uint8_t* r0 = src + (size_t)(y * f + a) * stride;
        const uint8_t* r1 = src + (size_t)(y * f + c) * stride;
        uint8_t* row = out.data() + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            const int x0 = x * f + a, x1 = x * f + c;
            row[dstX(x)] = (uint8_t)((r0[x0] + r0[x1] + r1[x0] + r1[x1]) >> 2);
        }
    }
}

int DownsampleFactorFor(int frameWidth) {
    const int E = frameWidth / 2;
    return std::max(1, (E + kMaxEyeWidth - 1) / kMaxEyeWidth);
}

// ---- policies ------------------------------------------------------------------------------
const char* PolicyName(SubjectPolicy p) {
    switch (p) {
        case SubjectPolicy::Nearest: return "nearest";
        case SubjectPolicy::Sharpness: return "sharp";
        case SubjectPolicy::Centre: return "centre";
        case SubjectPolicy::Focus: return "focus";
    }
    return "?";
}

bool ParsePolicy(const std::string& s, SubjectPolicy& out) {
    if (s == "nearest" || s == "near") out = SubjectPolicy::Nearest;
    else if (s == "sharp" || s == "sharpness") out = SubjectPolicy::Sharpness;
    else if (s == "centre" || s == "center") out = SubjectPolicy::Centre;
    else if (s == "focus") out = SubjectPolicy::Focus;
    else return false;
    return true;
}

// ---- estimator -------------------------------------------------------------------------------
void DisparityEstimator::Reset() {
    tracker_.Reset();
    track_.Reset();
    prev_.clear();
    prevW_ = prevH_ = 0;
    hasRange_ = false;
    lastBlocks_ = 0;
}

Result DisparityEstimator::Measure(const uint8_t* y, int stride, int w, int h,
                                   StereoLayout layout, SubjectPolicy policy,
                                   const float* focusUV, double nowMs, bool swapEyes) {
    if (layout != StereoLayout::SbsFull && layout != StereoLayout::SbsHalf) {
        Result r;
        r.rule = "mono";
        return r;
    }
    int ow = 0, oh = 0;
    DownsampleLuma(y, stride, w, h, DownsampleFactorFor(w), swapEyes, small_, ow, oh);
    return MeasureGray(small_.data(), ow, oh, policy, focusUV, nowMs);
}

Result DisparityEstimator::MeasureGray(const uint8_t* g, int W, int H, SubjectPolicy policy,
                                       const float* focusUV, double nowMs) {
    const auto t0 = std::chrono::steady_clock::now();
    Result r;
    const SbsGray img{g, W, H};
    const int E = img.E();
    r.eyeW = E;
    r.eyeH = H;
    if (E < 16 || H < 16) {
        r.rule = "none";
        return r;
    }
    // Policy / focus change: whatever was tracked belongs to the old subject.
    const bool focusChanged =
        focusUV && (focusUV[0] != lastFocus_[0] || focusUV[1] != lastFocus_[1]);
    if (policy != lastPolicy_ || (policy == SubjectPolicy::Focus && focusChanged)) {
        tracker_.Reset();
        track_.Reset();
        lastPolicy_ = policy;
    }
    if (focusUV) { lastFocus_[0] = focusUV[0]; lastFocus_[1] = focusUV[1]; }

    // Scene-cut detector: whole-frame mean |dI| vs the previous downsampled frame.
    if (prevW_ == W && prevH_ == H && !prev_.empty()) {
        int64_t acc = 0;
        const size_t n = (size_t)W * H;
        for (size_t i = 0; i < n; ++i) acc += std::abs((int)g[i] - (int)prev_[i]);
        r.sceneMad = (float)((double)acc / (double)n);
        if (r.sceneMad > kSceneCutMad) {
            // The tracked template belongs to the old scene. The median-of-3 hold is only
            // restarted below if the new frame measures: a covered camera must still hold.
            r.sceneCut = true;
            tracker_.Reset();
        }
    }
    prev_.assign(g, g + (size_t)W * H);
    prevW_ = W;
    prevH_ = H;

    SearchOptions o;
    Measurement m;
    blocks_.clear();
    bool searched = false;
    const char* rule = "none";
    if (policy == SubjectPolicy::Nearest || (policy == SubjectPolicy::Focus && focusUV)) {
        float fxy[2];
        const float* fp = nullptr;
        if (policy == SubjectPolicy::Focus) {
            fxy[0] = focusUV[0] * (float)E;
            fxy[1] = focusUV[1] * (float)H;
            fp = fxy;
        }
        m = tracker_.Measure(img, nowMs, o, fp, &blocks_);
        searched = !blocks_.empty() || m.method == Method::Mode;
        rule = m.ok ? MethodName(m.method) : "none";
        // The focus path runs no block search: the comfort clamp still needs the range,
        // so refresh it whenever the tracker did a full (non-track) measurement.
        if (m.method == Method::Focus) {
            blocks_ = BlockDisparities(img, o);
            searched = true;
        }
    } else {
        blocks_ = BlockDisparities(img, o);
        searched = true;
        SubjectPolicy eff = policy;
        bool fellThrough = false;
        if (policy == SubjectPolicy::Sharpness && !blocks_.empty()) {
            std::vector<float> sh;
            for (const Block& b : blocks_) sh.push_back(b.sharp);
            std::sort(sh.begin(), sh.end());
            const float med = sh[sh.size() >> 1], mx = sh.back();
            if (med <= 0.f || mx / med < 1.5f) {
                eff = SubjectPolicy::Centre;  // near-uniform sharpness: nothing stands out
                fellThrough = true;
            } else {
                const float q75 = sh[(size_t)std::floor(0.75f * (float)(sh.size() - 1))];
                std::vector<std::pair<float, float>> dv;
                float cx = 0, cy = 0, wsum = 0;
                for (const Block& b : blocks_)
                    if (b.sharp >= q75) {
                        dv.emplace_back(b.d, b.c);
                        cx += b.c * (b.x + 0.5f * Resolve(img, o).b);
                        cy += b.c * (b.y + 0.5f * Resolve(img, o).b);
                        wsum += b.c;
                    }
                if ((int)dv.size() >= o.minBlocks) {
                    m.ok = true;
                    m.d = WeightedMedian(dv);
                    m.x = cx / wsum;
                    m.y = cy / wsum;
                    m.c = wsum / (float)dv.size();
                    m.method = Method::Sharp;
                    rule = "sharp";
                }
            }
        }
        if (eff == SubjectPolicy::Centre && (int)blocks_.size() >= o.minBlocks) {
            const int b = Resolve(img, o).b;
            const float sigma = 0.2f;  // of the eye size
            std::vector<std::pair<float, float>> dv;
            float csum = 0;
            for (const Block& bl : blocks_) {
                const float u = (bl.x + 0.5f * b) / (float)E - 0.5f;
                const float v = (bl.y + 0.5f * b) / (float)H - 0.5f;
                const float wgt = bl.c * std::exp(-(u * u + v * v) / (2.f * sigma * sigma));
                dv.emplace_back(bl.d, wgt);
                csum += bl.c;
            }
            m.ok = true;
            m.d = WeightedMedian(dv);
            m.x = 0.5f * E;
            m.y = 0.5f * H;
            m.c = csum / (float)blocks_.size();
            m.method = Method::Centre;
            rule = fellThrough ? "sharp>centre" : "centre";
        }
    }
    if (searched) {
        lastBlocks_ = (int)blocks_.size();
        if (blocks_.size() >= 3) {
            std::vector<float> ds;
            for (const Block& b : blocks_) ds.push_back(b.d);
            nearFrac_ = Percentile(ds, 0.95f) / (float)E;
            farFrac_ = Percentile(ds, 0.05f) / (float)E;
            hasRange_ = true;
        } else {
            hasRange_ = false;
        }
    }
    r.ok = m.ok;
    r.rule = rule;
    r.ncc = m.c;
    r.rawFrac = m.ok ? m.d / (float)E : 0.f;
    r.focusU = m.x / (float)E;
    r.focusV = m.y / (float)H;
    if (r.sceneCut && m.ok) track_.Reset();  // a new scene: do not median it with the old
    r.hasValue = track_.Push(m.ok, m.d);
    r.disparityFrac = r.hasValue ? track_.Value() / (float)E : 0.f;
    r.blocksMatched = lastBlocks_;
    r.hasRange = hasRange_;
    r.nearFrac = nearFrac_;
    r.farFrac = farFrac_;
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
               .count();
    return r;
}

// ---- controller ------------------------------------------------------------------------------
float AutoConvergenceController::TargetFor(const Result& r, const char** clamp) const {
    if (clamp) *clamp = "";
    if (!r.hasValue) return 0.f;
    // The subject at zero parallax: displayed disparity = d + 2*shift = 0.
    float s = -0.5f * r.disparityFrac;
    if (cfg_.comfortClamp && r.hasRange) {
        // Rear first: the farthest content may sit at most rearBudget behind the plane
        // (farFrac + 2s >= -rear) — pull the scene forward if it does not.
        const float rearMin = 0.5f * (-cfg_.rearBudget - r.farFrac);
        if (s < rearMin) {
            s = rearMin;
            if (clamp) *clamp = "rear";
        }
        // Front wins: the nearest content may sit at most frontBudget in front
        // (nearFrac + 2s <= front).
        const float frontMax = 0.5f * (cfg_.frontBudget - r.nearFrac);
        if (s > frontMax) {
            s = frontMax;
            if (clamp) *clamp = "front";
        }
    }
    return std::max(-cfg_.maxShift, std::min(cfg_.maxShift, s));
}

void AutoConvergenceController::Update(const Result& r) {
    if (!r.hasValue) return;  // dropout with nothing held: keep the previous target
    const char* c = "";
    const float t = TargetFor(r, &c);
    clamp_ = c;
    if (!hasTarget_ || r.sceneCut) fast_ = true;
    target_ = t;
    hasTarget_ = true;
}

void AutoConvergenceController::Tick(double dt, bool enabled) {
    if (dt <= 0.0) return;
    if (enabled != wasEnabled_) {
        wasEnabled_ = enabled;
        fast_ = true;  // on: glide to the first target; off: glide home
        if (!enabled) { hasTarget_ = false; clamp_ = ""; }
    }
    const float goal = enabled && hasTarget_ ? target_ : 0.f;
    if (!enabled && value_ == 0.f) return;  // DEFAULT OFF stays exactly 0
    // Low-pass alpha per update at updateHz, as a continuous-time rate.
    const float a = 1.f - std::pow(1.f - cfg_.alpha, (float)(dt * cfg_.updateHz));
    float step = a * (goal - value_);
    if (!fast_) {
        const float lim = cfg_.ratePerSec * (float)dt;
        step = std::max(-lim, std::min(lim, step));
    }
    value_ += step;
    if (std::fabs(goal - value_) < 0.0005f) {
        fast_ = false;  // caught up once: the rate limit applies again
        if (!enabled && std::fabs(value_) < 0.0005f) value_ = 0.f;
    }
}

void AutoConvergenceController::Reset() {
    value_ = target_ = 0.f;
    hasTarget_ = false;
    fast_ = false;
    wasEnabled_ = false;
    clamp_ = "";
}

// ---- worker ----------------------------------------------------------------------------------
void AutoConvergenceWorker::Start() {
    if (thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = false;
        pending_ = false;
    }
    thread_ = std::thread([this] { Main(); });
}

void AutoConvergenceWorker::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

double AutoConvergenceWorker::Submit(const uint8_t* y, int stride, int w, int h, bool swapEyes,
                                     SubjectPolicy policy, const float* focusUV, double nowMs) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> local;
    {
        // Reuse the staging buffer's capacity: swap it out, fill, swap back.
        std::lock_guard<std::mutex> lk(m_);
        local.swap(staging_);
    }
    int ow = 0, oh = 0;
    DownsampleLuma(y, stride, w, h, DownsampleFactorFor(w), swapEyes, local, ow, oh);
    {
        std::lock_guard<std::mutex> lk(m_);
        staging_.swap(local);
        sw_ = ow;
        sh_ = oh;
        policy_ = policy;
        hasFocus_ = focusUV != nullptr;
        if (focusUV) { focus_[0] = focusUV[0]; focus_[1] = focusUV[1]; }
        nowMs_ = nowMs;
        pending_ = true;
    }
    cv_.notify_one();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
}

bool AutoConvergenceWorker::TakeResult(Result& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (!hasResult_) return false;
    out = result_;
    hasResult_ = false;
    return true;
}

void AutoConvergenceWorker::RequestReset() {
    std::lock_guard<std::mutex> lk(m_);
    resetReq_ = true;
}

void AutoConvergenceWorker::Main() {
    for (;;) {
        int w = 0, h = 0;
        SubjectPolicy policy;
        bool hasFocus;
        float focus[2];
        double now;
        bool reset;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stop_ || pending_; });
            if (stop_) return;
            work_.swap(staging_);  // the caller fills the other buffer next time
            pending_ = false;
            w = sw_;
            h = sh_;
            policy = policy_;
            hasFocus = hasFocus_;
            focus[0] = focus_[0];
            focus[1] = focus_[1];
            now = nowMs_;
            reset = resetReq_;
            resetReq_ = false;
        }
        if (reset) est_.Reset();
        if (work_.size() < (size_t)w * h || w <= 0) continue;
        const Result r = est_.MeasureGray(work_.data(), w, h, policy, hasFocus ? focus : nullptr, now);
        std::lock_guard<std::mutex> lk(m_);
        result_ = r;
        hasResult_ = true;
    }
}

}  // namespace mp::autoconv
