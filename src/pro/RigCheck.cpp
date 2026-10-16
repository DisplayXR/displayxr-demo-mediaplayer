// SPDX-License-Identifier: Apache-2.0
#include "pro/RigCheck.h"

#include "media/LiveProducer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace mp::rig {

namespace {

// Limited/full-range BT.709 Y'CbCr -> R'G'B' (0..1).
void YuvToRgb(float y, float u, float v, bool fullRange, float rgb[3]) {
    float yb, ub, vb;
    if (fullRange) {
        yb = y / 255.f;
        ub = (u - 128.f) / 255.f;
        vb = (v - 128.f) / 255.f;
    } else {
        yb = (y - 16.f) / 219.f;
        ub = (u - 128.f) / 224.f;
        vb = (v - 128.f) / 224.f;
    }
    rgb[0] = std::clamp(yb + 1.5748f * vb, 0.f, 1.f);
    rgb[1] = std::clamp(yb - 0.1873f * ub - 0.4681f * vb, 0.f, 1.f);
    rgb[2] = std::clamp(yb + 1.8556f * ub, 0.f, 1.f);
}

void ToLab(const float rgb[3], float lab[3]) {
    float lin[3];
    for (int i = 0; i < 3; ++i) {
        const float c = rgb[i];
        lin[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    const float X = (0.4124f * lin[0] + 0.3576f * lin[1] + 0.1805f * lin[2]) / 0.95047f;
    const float Y = (0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2]);
    const float Z = (0.0193f * lin[0] + 0.1192f * lin[1] + 0.9505f * lin[2]) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    lab[0] = 116.f * f(Y) - 16.f;
    lab[1] = 500.f * (f(X) - f(Y));
    lab[2] = 200.f * (f(Y) - f(Z));
}

std::string WallClock(double /*steadySec*/) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

// Normalised cross-correlation of an 8-bit-range float block of the left eye (pre-centred
// as `tpl`, with its norm) against the right eye at (x, y).
float Ncc(const std::vector<float>& R, int eyeW, const float* tpl, float tplNorm, int bs, int x, int y) {
    float sum = 0, sum2 = 0, cross = 0;
    for (int j = 0; j < bs; ++j) {
        const float* r = R.data() + (size_t)(y + j) * eyeW + x;
        const float* t = tpl + j * bs;
        for (int i = 0; i < bs; ++i) {
            const float v = r[i];
            sum += v;
            sum2 += v * v;
            cross += v * t[i];
        }
    }
    const float n = (float)(bs * bs);
    const float var = sum2 - sum * sum / n;
    if (var <= 1e-3f || tplNorm <= 1e-3f) return -1.f;
    return cross / (std::sqrt(var) * tplNorm);  // tpl is mean-free, so sum*mean(tpl) = 0
}

// Parabola vertex offset from three samples around a peak, in [-0.5, 0.5].
float SubPixel(float a, float b, float c) {
    const float den = a - 2.f * b + c;
    if (std::fabs(den) < 1e-6f) return 0.f;
    return std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f);
}

}  // namespace

float DeltaE76(const float a[3], const float b[3]) {
    float la[3], lb[3];
    ToLab(a, la);
    ToLab(b, lb);
    return std::sqrt((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) +
                     (la[2] - lb[2]) * (la[2] - lb[2]));
}

bool Downsample(const FrameRing::Frame& f, StereoLayout layout, int analysisW, EyePair& out, bool swapEyes) {
    if (layout != StereoLayout::SbsFull && layout != StereoLayout::SbsHalf) return false;
    if (f.width < 64 || f.height < 32 || f.plane[0].empty()) return false;
    if (f.format != PixFormat::NV12 && f.format != PixFormat::I420 && f.format != PixFormat::P016) return false;
    const bool p16 = f.format == PixFormat::P016;  // 16-bit samples, MSB-aligned
    auto lumaAt = [&](size_t i) -> float {
        return p16 ? (float)reinterpret_cast<const uint16_t*>(f.plane[0].data())[i] / 256.0f
                   : (float)f.plane[0][i];
    };
    const int fullEye = f.width / 2;
    const int aw = std::min(analysisW, fullEye);
    const float sx = (float)fullEye / (float)aw;
    const int ah = std::max(16, (int)std::lround(f.height / sx));
    const float sy = (float)f.height / (float)ah;
    out.eyeW = aw;
    out.h = ah;
    out.fullEyeW = (float)fullEye;
    out.fullH = (float)f.height;
    out.xStretch = layout == StereoLayout::SbsHalf ? 2.f : 1.f;  // half-SBS eyes are squeezed
    out.left.resize((size_t)aw * ah);
    out.right.resize((size_t)aw * ah);
    const int W = f.width;
    for (int eye = 0; eye < 2; ++eye) {
        std::vector<float>& dst = eye == 0 ? out.left : out.right;
        // the eye as SHOWN: with the eyes swapped the right half feeds the left eye
        const int x0 = (eye ^ (swapEyes ? 1 : 0)) * fullEye;
        for (int y = 0; y < ah; ++y) {
            const int sy0 = std::min(f.height - 2, (int)(y * sy));
            const size_t r0 = (size_t)sy0 * W, r1 = r0 + W;
            float* d = dst.data() + (size_t)y * aw;
            for (int x = 0; x < aw; ++x) {
                const size_t sx0 = (size_t)x0 + std::min(fullEye - 2, (int)(x * sx));
                d[x] = 0.25f * (lumaAt(r0 + sx0) + lumaAt(r0 + sx0 + 1) + lumaAt(r1 + sx0) + lumaAt(r1 + sx0 + 1));
            }
        }
        // mean colour over the central 80 %, on a coarse grid of chroma samples
        double sy_ = 0, su = 0, sv = 0;
        int n = 0;
        const int cw = W / 2, chh = f.height / 2;
        for (int y = f.height / 10; y < f.height * 9 / 10; y += 16) {
            for (int x = fullEye / 10; x < fullEye * 9 / 10; x += 16) {
                const int gx = x0 + x;
                sy_ += lumaAt((size_t)y * W + gx);
                if (p16) {
                    const uint16_t* uv = reinterpret_cast<const uint16_t*>(f.plane[1].data()) + (size_t)(y / 2) * W + (gx & ~1);
                    su += uv[0] / 256.0;
                    sv += uv[1] / 256.0;
                } else if (f.format == PixFormat::NV12) {
                    const uint8_t* uv = f.plane[1].data() + (size_t)(y / 2) * W + (gx & ~1);
                    su += uv[0];
                    sv += uv[1];
                } else {
                    su += f.plane[1][(size_t)(y / 2) * cw + gx / 2];
                    sv += f.plane[2][(size_t)(y / 2) * cw + gx / 2];
                }
                ++n;
            }
        }
        (void)chh;
        if (n > 0) YuvToRgb((float)(sy_ / n), (float)(su / n), (float)(sv / n), f.fullRange, out.meanRgb[eye]);
    }
    return true;
}

Result Measure(const EyePair& e, const Options& o) {
    Result r;
    const int bs = o.block, W = e.eyeW, H = e.h;
    if (W < 4 * bs || H < 4 * bs) return r;
    const int R = std::max(2, (int)std::lround(o.maxParallaxPct / 100.f * W));
    const int D = o.maxDy;
    std::vector<float> tpl((size_t)bs * bs);
    std::vector<float> dxs, dys, xs;
    for (int by = D + 2; by + bs + D + 2 <= H; by += o.step) {
        for (int bx = 2; bx + bs + 2 <= W; bx += o.step) {
            // template: centred left block
            float mean = 0;
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) mean += e.left[(size_t)(by + j) * W + bx + i];
            mean /= (float)(bs * bs);
            float norm = 0;
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) {
                    const float v = e.left[(size_t)(by + j) * W + bx + i] - mean;
                    tpl[(size_t)j * bs + i] = v;
                    norm += v * v;
                }
            const float sd = std::sqrt(norm / (float)(bs * bs));
            if (sd < o.minTexture) continue;
            norm = std::sqrt(norm);
            const int dxLo = std::max(-R, -bx), dxHi = std::min(R, W - bs - bx);
            const int dyLo = std::max(-D, -by), dyHi = std::min(D, H - bs - by);
            // coarse (step 2), then refine (+-2, step 1)
            float best = -2.f;
            int bdx = 0, bdy = 0;
            for (int dy = dyLo; dy <= dyHi; dy += 2)
                for (int dx = dxLo; dx <= dxHi; dx += 2) {
                    const float c = Ncc(e.right, W, tpl.data(), norm, bs, bx + dx, by + dy);
                    if (c > best) {
                        best = c;
                        bdx = dx;
                        bdy = dy;
                    }
                }
            const int cx = bdx, cy = bdy;
            for (int dy = std::max(dyLo, cy - 2); dy <= std::min(dyHi, cy + 2); ++dy)
                for (int dx = std::max(dxLo, cx - 2); dx <= std::min(dxHi, cx + 2); ++dx) {
                    const float c = Ncc(e.right, W, tpl.data(), norm, bs, bx + dx, by + dy);
                    if (c > best) {
                        best = c;
                        bdx = dx;
                        bdy = dy;
                    }
                }
            if (best < o.minNcc) continue;
            // uniqueness: no rival as good >= 4 px away along the row (periodic texture)
            float rival = -2.f;
            for (int dx = dxLo; dx <= dxHi; dx += 2)
                if (std::abs(dx - bdx) >= 4)
                    rival = std::max(rival, Ncc(e.right, W, tpl.data(), norm, bs, bx + dx, by + bdy));
            if (rival > best - 0.05f) continue;
            float fx = 0, fy = 0;
            if (bdx > dxLo && bdx < dxHi)
                fx = SubPixel(Ncc(e.right, W, tpl.data(), norm, bs, bx + bdx - 1, by + bdy), best,
                              Ncc(e.right, W, tpl.data(), norm, bs, bx + bdx + 1, by + bdy));
            if (bdy > dyLo && bdy < dyHi)
                fy = SubPixel(Ncc(e.right, W, tpl.data(), norm, bs, bx + bdx, by + bdy - 1), best,
                              Ncc(e.right, W, tpl.data(), norm, bs, bx + bdx, by + bdy + 1));
            dxs.push_back((float)bdx + fx);
            dys.push_back((float)bdy + fy);
            xs.push_back((float)bx + bs * 0.5f);
            r.matchX.push_back(((float)bx + bs * 0.5f) / (float)W);
            r.matchY.push_back(((float)by + bs * 0.5f) / (float)H);
        }
    }
    // Outlier cut (median +- k x MAD) before any statistic: a block that locked onto the
    // wrong repeat of a texture gives one wild dx / dy, and with few matches the 2nd / 98th
    // percentiles ARE the extremes. dx keeps a wide floor (the scene's real depth spread,
    // e.g. a -2 % foreground over a 0 % background, must survive); dy should be one value
    // plus roll, so its floor is tight.
    auto median = [](std::vector<float> v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    if ((int)dxs.size() >= o.minMatches) {
        const float mdx = median(dxs), mdy = median(dys);
        std::vector<float> ax(dxs.size()), ay(dys.size());
        for (size_t i = 0; i < dxs.size(); ++i) {
            ax[i] = std::fabs(dxs[i] - mdx);
            ay[i] = std::fabs(dys[i] - mdy);
        }
        const float tx = std::max(5.f * 1.4826f * median(ax), 0.03f * (float)W);
        const float ty = std::max(4.f * 1.4826f * median(ay), 1.0f);
        size_t k = 0;
        for (size_t i = 0; i < dxs.size(); ++i) {
            if (ax[i] > tx || ay[i] > ty) continue;
            dxs[k] = dxs[i];
            dys[k] = dys[i];
            xs[k] = xs[i];
            r.matchX[k] = r.matchX[i];
            r.matchY[k] = r.matchY[i];
            ++k;
        }
        dxs.resize(k);
        dys.resize(k);
        xs.resize(k);
        r.matchX.resize(k);
        r.matchY.resize(k);
    }
    r.matches = (int)dxs.size();
    if (r.matches < std::max(4, o.minMatches)) return r;
    r.ok = true;
    // parallax (% of eye width): x_right - x_left = dx
    r.parallaxPct.resize(dxs.size());
    for (size_t i = 0; i < dxs.size(); ++i) r.parallaxPct[i] = dxs[i] / (float)W * 100.f;
    std::vector<float> sorted = r.parallaxPct;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](float q) { return sorted[(size_t)std::lround(q * (float)(sorted.size() - 1))]; };
    r.nearPct = pct(0.02f);
    r.farPct = pct(0.98f);
    const float scaleY = e.fullH / (float)H;
    r.verticalPx = median(dys) * scaleY;
    // roll: least-squares slope of dy over x (both in analysis px)
    double mx = 0, my = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
        mx += xs[i];
        my += dys[i];
    }
    mx /= xs.size();
    my /= xs.size();
    double sxy = 0, sxx = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
        sxy += (xs[i] - mx) * (dys[i] - my);
        sxx += (xs[i] - mx) * (xs[i] - mx);
    }
    // dy is in rows of H, x in columns of W; convert both to full-res DISPLAYED px before
    // the angle (a half-SBS eye is shown twice as wide as it is stored)
    const double slope = sxx > 0 ? (sxy / sxx) * (scaleY / (e.fullEyeW * e.xStretch / (float)W)) : 0.0;
    r.rollDeg = (float)(std::atan(slope) * 180.0 / 3.14159265358979);
    r.deltaE = DeltaE76(e.meanRgb[0], e.meanRgb[1]);
    return r;
}

namespace {

// Mean absolute difference of two analysis-size luma images, b sampled at (x + sx, y + sy),
// over the overlap minus a margin; each image's mean over that region is removed first so a
// gain / offset mismatch between the eyes does not count. Every 2nd pixel: plenty, cheap.
float Mad(const std::vector<float>& a, const std::vector<float>& b, int W, int H, int sx, int sy) {
    const int m = 4;
    const int x0 = std::max(m, m - sx), x1 = std::min(W - m, W - m - sx);
    const int y0 = std::max(m, m - sy), y1 = std::min(H - m, H - m - sy);
    if (x1 - x0 < 16 || y1 - y0 < 16) return -1.f;
    double ma = 0, mb = 0;
    int n = 0;
    for (int y = y0; y < y1; y += 2)
        for (int x = x0; x < x1; x += 2) {
            ma += a[(size_t)y * W + x];
            mb += b[(size_t)(y + sy) * W + x + sx];
            ++n;
        }
    ma /= n;
    mb /= n;
    double d = 0;
    for (int y = y0; y < y1; y += 2)
        for (int x = x0; x < x1; x += 2)
            d += std::fabs((a[(size_t)y * W + x] - ma) - (b[(size_t)(y + sy) * W + x + sx] - mb));
    return (float)(d / n);
}

}  // namespace

int MeasureEyeSync(const std::vector<EyePair>& seq, float shiftX, float shiftY, int* lateEye) {
    if (lateEye) *lateEye = 0;
    const int n = (int)seq.size();
    if (n < 2) return -1;
    const EyePair& cur = seq.back();
    const int W = cur.eyeW, H = cur.h;
    for (const EyePair& e : seq)
        if (e.eyeW != W || e.h != H || e.left.size() != (size_t)W * H || e.right.size() != (size_t)W * H) return -1;
    // Motion: without it every candidate looks alike and there is nothing to measure.
    const float motion = Mad(cur.left, seq[n - 2].left, W, H, 0, 0);
    if (motion < 0.4f) return -1;
    const int sx = (int)std::lround(shiftX), sy = (int)std::lround(shiftY);
    // Candidates: right(now) vs left(now - k) -> the right eye is k late; left(now) vs
    // right(now - k) -> the left eye is k late. Right sampled at +shift in both.
    struct Cand {
        float score;
        int frames, late;
    };
    std::vector<Cand> c;
    for (int k = 0; k < n; ++k) {
        const float a = Mad(seq[n - 1 - k].left, cur.right, W, H, sx, sy);
        if (a >= 0) c.push_back({a, k, k == 0 ? 0 : +1});
        if (k == 0) continue;
        const float b = Mad(cur.left, seq[n - 1 - k].right, W, H, sx, sy);
        if (b >= 0) c.push_back({b, k, -1});
    }
    if (c.size() < 2) return -1;
    std::sort(c.begin(), c.end(), [](const Cand& x, const Cand& y) { return x.score < y.score; });
    // A clear winner only: the runner-up must be clearly worse.
    if (c[1].score - c[0].score < 0.15f * c[1].score) return -1;
    if (lateEye) *lateEye = c[0].late;
    return c[0].frames;
}

// ---------------------------------------------------------------------------------------

RigMonitor::RigMonitor() { worker_ = std::thread([this] { Worker(); }); }

RigMonitor::~RigMonitor() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void RigMonitor::Submit(const FrameRing::Frame& f, StereoLayout layout, int eyeOffsetFrames,
                        const std::string& timecode, bool swapEyes) {
    const double now = LiveProducer::NowSeconds();
    if (burst_.empty()) {
        std::lock_guard<std::mutex> lk(m_);
        if (pending_ || now - lastSubmit_ < 0.25) return;  // ~4 measurements a second
    } else if (f.serial != burstLastSerial_ + 1 || layout != burstLayout_ || swapEyes != burstSwap_) {
        burst_.clear();  // a dropped frame or a change: the burst is no longer consecutive
    }
    EyePair e;
    if (!Downsample(f, layout, 640, e, swapEyes)) {
        burst_.clear();
        return;
    }
    if (burst_.empty()) burstStart_ = now;
    burst_.push_back(std::move(e));
    burstLastSerial_ = f.serial;
    burstLayout_ = layout;
    burstSwap_ = swapEyes;
    // Hand over once the burst is complete - or, when frames keep dropping, after 0.5 s
    // with the latest frame alone (spatial checks only, eye sync not measured).
    const bool complete = (int)burst_.size() >= kSyncFrames;
    if (!complete && now - burstStart_ < 0.5) return;
    if (!complete) burst_.erase(burst_.begin(), burst_.end() - 1);
    std::lock_guard<std::mutex> lk(m_);
    work_ = std::move(burst_);
    burst_.clear();
    pendingEyeOffset_ = eyeOffsetFrames;
    pendingTimecode_ = timecode;
    pending_ = true;
    lastSubmit_ = now;
    cv_.notify_one();
}

void RigMonitor::Worker() {
    std::unique_lock<std::mutex> lk(m_);
    while (true) {
        cv_.wait(lk, [this] { return stop_ || pending_; });
        if (stop_) return;
        std::vector<EyePair> seq = std::move(work_);
        work_.clear();
        const uint64_t gen = generation_;
        const int eyeOffset = pendingEyeOffset_;
        const std::string tc = pendingTimecode_;
        lk.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        Result r = seq.empty() ? Result{} : Measure(seq.back());
        if (r.ok && seq.size() >= 2) {
            // remove the rig's mean offset first: median parallax and the vertical error
            std::vector<float> p = r.parallaxPct;
            std::nth_element(p.begin(), p.begin() + p.size() / 2, p.end());
            const EyePair& e = seq.back();
            const float shiftX = p[p.size() / 2] / 100.f * (float)e.eyeW;
            const float shiftY = e.fullH > 0 ? r.verticalPx * (float)e.h / e.fullH : 0.f;
            r.eyeSyncMeasuredFrames = MeasureEyeSync(seq, shiftX, shiftY, &r.eyeSyncLateEye);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        lk.lock();
        pending_ = false;
        if (gen != generation_) continue;  // Reset() ran meanwhile: this was the old input
        measureMs_ = ms;
        last_ = r;
        Sample s;
        s.t = LiveProducer::NowSeconds();
        s.ok = r.ok;
        s.nearPct = r.nearPct;
        s.farPct = r.farPct;
        s.verticalPx = r.verticalPx;
        s.deltaE = r.deltaE;
        s.eyeOffsetFrames = eyeOffset;
        s.eyeSyncMeasured = r.eyeSyncMeasuredFrames;
        s.eyeSyncLateEye = r.eyeSyncLateEye;
        AddSampleLocked(s, tc);
    }
}

void RigMonitor::AddSample(const Sample& s, const std::string& tc) {
    std::lock_guard<std::mutex> lk(m_);
    AddSampleLocked(s, tc);
}

void RigMonitor::AddSampleLocked(const Sample& s, const std::string& tc) {
    history_.push_back(s);
    while (!history_.empty() && s.t - history_.front().t > 120.0) history_.pop_front();
    UpdateAlerts(s, tc);
}

namespace {
// The alert texts, per run: near, far, vertical, colour.
const char* const kClosedFmt[4] = {
    "Foreground at %+.1f%% for %.0f s (near limit %+.1f%%)",
    "Background at %+.1f%% for %.0f s (far limit %+.1f%%)",
    "Vertical error %.1f px for %.0f s (limit %.1f px)",
    "Colour mismatch dE %.1f for %.0f s (limit %.1f)",
};
const char* const kOpenFmt[4] = {
    "Foreground at %+.1f%% for %.0f s (now)",
    "Background at %+.1f%% for %.0f s (now)",
    "Vertical error %.1f px for %.0f s (now)",
    "Colour mismatch dE %.1f for %.0f s (now)",
};
// An open run is closed when the picture has not measured for this long (a cut to black,
// a lost signal): it would otherwise stay "now" until the next good sample.
constexpr double kAlertExpireSec = 2.0;
}  // namespace

// Under m_. Ends run i at `end`: kept as an alert when it lasted >= 0.5 s.
void RigMonitor::CloseRun(int i, double end, const std::string& endTc) {
    Run& r = runs_[i];
    if (r.since < 0) return;
    const double dur = end - r.since;
    if (dur >= 0.5) {
        const float limits[] = {budget_.nearPct, budget_.farPct, budget_.maxVerticalPx, budget_.maxDeltaE};
        Alert a;
        a.start = r.since;
        a.end = end;
        char b[160];
        std::snprintf(b, sizeof(b), kClosedFmt[i], r.worst, std::max(1.0, std::round(dur)), limits[i]);
        a.what = b;
        a.timecode = r.tc;
        a.endTimecode = endTc;
        a.wallClock = r.clock;
        alerts_.push_back(a);
        if (alerts_.size() > 20) alerts_.erase(alerts_.begin());
    }
    r.since = -1;
}

// Under m_. A budget overrun becomes an alert once it is over and lasted >= 0.5 s. Samples
// that did not measure leave the runs alone, unless they go on for kAlertExpireSec: then
// every open run closes at the last good sample, with its timecode.
void RigMonitor::UpdateAlerts(const Sample& s, const std::string& tc) {
    if (!s.ok) {
        if (notOkSince_ < 0) notOkSince_ = s.t;
        if (s.t - notOkSince_ >= kAlertExpireSec && lastGoodT_ >= 0)
            for (int i = 0; i < 4; ++i) CloseRun(i, lastGoodT_, lastGoodTc_);
        return;
    }
    notOkSince_ = -1;
    lastGoodT_ = s.t;
    lastGoodTc_ = tc;
    const bool over[4] = {s.nearPct < budget_.nearPct, s.farPct > budget_.farPct,
                          std::fabs(s.verticalPx) > budget_.maxVerticalPx, s.deltaE > budget_.maxDeltaE};
    const float value[4] = {s.nearPct, s.farPct, std::fabs(s.verticalPx), s.deltaE};
    for (int i = 0; i < 4; ++i) {
        Run& r = runs_[i];
        if (over[i]) {
            if (r.since < 0) {
                r.since = s.t;
                r.worst = value[i];
                r.tc = tc;
                r.clock = WallClock(s.t);
            } else {
                r.worst = i == 0 ? std::min(r.worst, value[i]) : std::max(r.worst, value[i]);  // near: lower is worse
            }
        } else {
            CloseRun(i, s.t, tc);
        }
    }
}

void RigMonitor::SetBudget(const Budget& b) {
    std::lock_guard<std::mutex> lk(m_);
    budget_ = b;
}

Budget RigMonitor::GetBudget() const {
    std::lock_guard<std::mutex> lk(m_);
    return budget_;
}

void RigMonitor::Reset() {
    std::lock_guard<std::mutex> lk(m_);
    ++generation_;
    history_.clear();
    alerts_.clear();
    last_ = Result{};
    for (Run& r : runs_) r.since = -1;
    lastGoodT_ = notOkSince_ = -1.0;
    lastGoodTc_.clear();
}

Result RigMonitor::Last() const {
    std::lock_guard<std::mutex> lk(m_);
    return last_;
}

std::vector<Sample> RigMonitor::History(double seconds) const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Sample> out;
    if (history_.empty()) return out;
    const double from = history_.back().t - seconds;
    for (const Sample& s : history_)
        if (s.t >= from) out.push_back(s);
    return out;
}

std::vector<Alert> RigMonitor::Alerts() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Alert> a = alerts_;
    // an overrun still in progress shows as an open alert
    const double now = history_.empty() ? 0.0 : history_.back().t;
    for (int i = 0; i < 4; ++i) {
        const Run& r = runs_[i];
        if (r.since < 0 || now - r.since < 0.5) continue;
        Alert x;
        x.start = r.since;
        x.end = -1.0;
        char b[160];
        std::snprintf(b, sizeof(b), kOpenFmt[i], r.worst, now - r.since);
        x.what = b;
        x.timecode = r.tc;
        x.wallClock = r.clock;
        a.push_back(x);
    }
    return a;
}

bool RigMonitor::Active(double now) const {
    std::lock_guard<std::mutex> lk(m_);
    return !history_.empty() && now - history_.back().t < 2.0;
}

double RigMonitor::LastMeasureMs() const {
    std::lock_guard<std::mutex> lk(m_);
    return measureMs_;
}

}  // namespace mp::rig
