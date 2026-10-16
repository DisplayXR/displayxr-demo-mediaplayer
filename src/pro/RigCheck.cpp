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

bool Downsample(const FrameRing::Frame& f, StereoLayout layout, int analysisW, EyePair& out) {
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
    out.left.resize((size_t)aw * ah);
    out.right.resize((size_t)aw * ah);
    const int W = f.width;
    for (int eye = 0; eye < 2; ++eye) {
        std::vector<float>& dst = eye == 0 ? out.left : out.right;
        const int x0 = eye * fullEye;
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
    r.matches = (int)dxs.size();
    if (r.matches < 4) return r;
    r.ok = true;
    // parallax (% of eye width): x_right - x_left = dx
    r.parallaxPct.resize(dxs.size());
    for (size_t i = 0; i < dxs.size(); ++i) r.parallaxPct[i] = dxs[i] / (float)W * 100.f;
    std::vector<float> sorted = r.parallaxPct;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](float q) { return sorted[(size_t)std::lround(q * (float)(sorted.size() - 1))]; };
    r.nearPct = pct(0.02f);
    r.farPct = pct(0.98f);
    std::vector<float> dy = dys;
    std::nth_element(dy.begin(), dy.begin() + dy.size() / 2, dy.end());
    const float scaleY = e.fullH / (float)H;
    r.verticalPx = dy[dy.size() / 2] * scaleY;
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
    // dy is in rows of H, x in columns of W; convert both to full-res px before the angle
    const double slope = sxx > 0 ? (sxy / sxx) * (scaleY / (e.fullEyeW / (float)W)) : 0.0;
    r.rollDeg = (float)(std::atan(slope) * 180.0 / 3.14159265358979);
    r.deltaE = DeltaE76(e.meanRgb[0], e.meanRgb[1]);
    return r;
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
                        const std::string& timecode) {
    const double now = LiveProducer::NowSeconds();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (pending_ || now - lastSubmit_ < 0.25) return;  // ~4 measurements a second
    }
    EyePair e;
    if (!Downsample(f, layout, 640, e)) return;
    std::lock_guard<std::mutex> lk(m_);
    work_ = std::move(e);
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
        EyePair e = std::move(work_);
        const int eyeOffset = pendingEyeOffset_;
        const std::string tc = pendingTimecode_;
        lk.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        Result r = Measure(e);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        lk.lock();
        pending_ = false;
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
        history_.push_back(s);
        while (!history_.empty() && s.t - history_.front().t > 120.0) history_.pop_front();
        UpdateAlerts(s, tc);
    }
}

// Under m_. A budget overrun becomes an alert once it is over and lasted >= 0.5 s.
void RigMonitor::UpdateAlerts(const Sample& s, const std::string& tc) {
    if (!s.ok) return;
    struct Run {
        bool over;
        float value;
        double& since;
        float& worst;
        std::string& tcStart;
        std::string& clock;
        bool lowerIsWorse;
        const char* fmt;
    };
    Run runs[] = {
        {s.nearPct < budget_.nearPct, s.nearPct, nearSince_, nearWorst_, nearTc_, nearClock_, true,
         "Foreground at %+.1f%% for %.0f s (near limit %+.1f%%)"},
        {s.farPct > budget_.farPct, s.farPct, farSince_, farWorst_, farTc_, farClock_, false,
         "Background at %+.1f%% for %.0f s (far limit %+.1f%%)"},
        {std::fabs(s.verticalPx) > budget_.maxVerticalPx, std::fabs(s.verticalPx), vertSince_, vertWorst_, vertTc_,
         vertClock_, false, "Vertical error %.1f px for %.0f s (limit %.1f px)"},
        {s.deltaE > budget_.maxDeltaE, s.deltaE, colSince_, colWorst_, colTc_, colClock_, false,
         "Colour mismatch dE %.1f for %.0f s (limit %.1f)"},
    };
    const float limits[] = {budget_.nearPct, budget_.farPct, budget_.maxVerticalPx, budget_.maxDeltaE};
    for (int i = 0; i < 4; ++i) {
        Run& r = runs[i];
        if (r.over) {
            if (r.since < 0) {
                r.since = s.t;
                r.worst = r.value;
                r.tcStart = tc;
                r.clock = WallClock(s.t);
            } else {
                r.worst = r.lowerIsWorse ? std::min(r.worst, r.value) : std::max(r.worst, r.value);
            }
        } else if (r.since >= 0) {
            const double dur = s.t - r.since;
            if (dur >= 0.5) {
                Alert a;
                a.start = r.since;
                a.end = s.t;
                char b[160];
                std::snprintf(b, sizeof(b), r.fmt, r.worst, std::max(1.0, std::round(dur)), limits[i]);
                a.what = b;
                a.timecode = r.tcStart;
                a.wallClock = r.clock;
                alerts_.push_back(a);
                if (alerts_.size() > 20) alerts_.erase(alerts_.begin());
            }
            r.since = -1;
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
    history_.clear();
    alerts_.clear();
    last_ = Result{};
    nearSince_ = farSince_ = vertSince_ = colSince_ = -1;
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
    auto open = [&](double since, const char* what, float worst, const std::string& tc, const std::string& clock) {
        if (since < 0 || now - since < 0.5) return;
        Alert x;
        x.start = since;
        x.end = -1.0;
        char b[160];
        std::snprintf(b, sizeof(b), what, worst, now - since);
        x.what = b;
        x.timecode = tc;
        x.wallClock = clock;
        a.push_back(x);
    };
    open(nearSince_, "Foreground at %+.1f%% for %.0f s (now)", nearWorst_, nearTc_, nearClock_);
    open(farSince_, "Background at %+.1f%% for %.0f s (now)", farWorst_, farTc_, farClock_);
    open(vertSince_, "Vertical error %.1f px for %.0f s (now)", vertWorst_, vertTc_, vertClock_);
    open(colSince_, "Colour mismatch dE %.1f for %.0f s (now)", colWorst_, colTc_, colClock_);
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
