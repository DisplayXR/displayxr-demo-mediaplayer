// SPDX-License-Identifier: Apache-2.0
#include "pro/RigCheck.h"

#include "media/LiveProducer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <iterator>
#include <thread>

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
    // The work is the same per output row and per eye, so it runs on several threads and
    // the render thread waits for them: every analysis pixel and the mean colours are
    // computed exactly as on one thread (each mean on one thread, so its sum order holds).
    // At 7680x2160 SBS this was 8-9 ms a call (max ~24) on the render thread, ~9 calls a
    // second during bursts (#120 review, 2026-10-19).
    auto lumaRows = [&](int eye, int yBegin, int yEnd) {
        std::vector<float>& dst = eye == 0 ? out.left : out.right;
        // the eye as SHOWN: with the eyes swapped the right half feeds the left eye
        const int x0 = (eye ^ (swapEyes ? 1 : 0)) * fullEye;
        // Integer sums: the samples are integers (P016 scaled by 1/256 at the end), every
        // partial sum stays under 2^24, so the result is the float sum bit for bit.
        std::vector<uint32_t> colSum((size_t)fullEye);
        // A box over each analysis pixel's whole footprint (sx x sy source pixels). Sampling
        // a 2x2 corner of a 3x3 footprint, as this did, skips five source pixels in nine:
        // an edge moving through the skipped rows does not move in the analysis image,
        // so sub-pixel motion is quantised and a frame of motion at 48p (~0.3 px here)
        // vanished - the eye-sync reading split between the true delay and its neighbour
        // or read "in step" (tmp-mp-pro, 2026-10-18).
        for (int y = yBegin; y < yEnd; ++y) {
            const int ya = std::min(f.height - 1, (int)(y * sy));
            const int yb = std::max(ya + 1, std::min(f.height, (int)((y + 1) * sy)));
            std::fill(colSum.begin(), colSum.end(), 0u);
            for (int yy = ya; yy < yb; ++yy) {
                const size_t r = (size_t)yy * W + x0;
                if (p16) {
                    const uint16_t* src = reinterpret_cast<const uint16_t*>(f.plane[0].data()) + r;
                    for (int x = 0; x < fullEye; ++x) colSum[x] += src[x];
                } else {
                    const uint8_t* src = f.plane[0].data() + r;
                    for (int x = 0; x < fullEye; ++x) colSum[x] += src[x];
                }
            }
            const float unit = p16 ? 1.0f / 256.0f : 1.0f;
            float* d = dst.data() + (size_t)y * aw;
            for (int x = 0; x < aw; ++x) {
                const int xa = std::min(fullEye - 1, (int)(x * sx));
                const int xb = std::max(xa + 1, std::min(fullEye, (int)((x + 1) * sx)));
                uint32_t acc = 0;
                for (int xx = xa; xx < xb; ++xx) acc += colSum[xx];
                d[x] = ((float)acc * unit) / (float)((xb - xa) * (yb - ya));
            }
        }
    };
    // Mean colour over the central 80 %: whole rows, every 7th. A sparse grid aliases with
    // periodic content (a 16 px grid on a drifting 160 px grating read one eye 15 % brighter
    // for a burst now and then, and the colour alert latched that peak - tmp-mp-pro
    // 2026-10-18). Summed in integers per chunk of rows: the samples are integers (P016 in
    // 1/256 steps), so the total is exact and equals the old double sum bit for bit.
    struct Sums {
        uint64_t y = 0, u = 0, v = 0;
        int64_t n = 0;
    };
    const int rowFirst = f.height / 10, rowEnd = f.height * 9 / 10;
    const int rowCount = rowFirst < rowEnd ? (rowEnd - rowFirst + 6) / 7 : 0;
    const int xFirst = fullEye / 10, xEnd = fullEye * 9 / 10;
    const int cw = W / 2;
    auto meanRows = [&](int eye, int kBegin, int kEnd) {
        const int x0 = (eye ^ (swapEyes ? 1 : 0)) * fullEye;
        Sums s;
        for (int k = kBegin; k < kEnd; ++k) {
            const int y = rowFirst + 7 * k;
            if (p16) {
                const uint16_t* Y = reinterpret_cast<const uint16_t*>(f.plane[0].data()) + (size_t)y * W;
                const uint16_t* UV = reinterpret_cast<const uint16_t*>(f.plane[1].data()) + (size_t)(y / 2) * W;
                for (int x = xFirst; x < xEnd; ++x) {
                    const int gx = x0 + x;
                    s.y += Y[gx];
                    s.u += UV[gx & ~1];
                    s.v += UV[(gx & ~1) + 1];
                }
            } else if (f.format == PixFormat::NV12) {
                const uint8_t* Y = f.plane[0].data() + (size_t)y * W;
                const uint8_t* UV = f.plane[1].data() + (size_t)(y / 2) * W;
                for (int x = xFirst; x < xEnd; ++x) {
                    const int gx = x0 + x;
                    s.y += Y[gx];
                    s.u += UV[gx & ~1];
                    s.v += UV[(gx & ~1) + 1];
                }
            } else {
                const uint8_t* Y = f.plane[0].data() + (size_t)y * W;
                const uint8_t* U = f.plane[1].data() + (size_t)(y / 2) * cw;
                const uint8_t* V = f.plane[2].data() + (size_t)(y / 2) * cw;
                for (int x = xFirst; x < xEnd; ++x) {
                    const int gx = x0 + x;
                    s.y += Y[gx];
                    s.u += U[gx / 2];
                    s.v += V[gx / 2];
                }
            }
            s.n += std::max(0, xEnd - xFirst);
        }
        return s;
    };
    // Threads per eye: up to 4. MEDIAPLAYER_RIG_THREADS=<n> pins it (tests compare 1 vs n).
    int chunks = std::clamp((int)std::thread::hardware_concurrency() / 2, 1, 4);
    if (const char* t = std::getenv("MEDIAPLAYER_RIG_THREADS")) chunks = std::clamp(std::atoi(t), 1, 16);
    std::vector<std::future<void>> jobs;
    std::vector<std::future<Sums>> means[2];
    for (int eye = 0; eye < 2; ++eye)
        for (int c = 0; c < chunks; ++c) {
            means[eye].push_back(std::async(std::launch::async, meanRows, eye, rowCount * c / chunks,
                                            rowCount * (c + 1) / chunks));
            const int y0 = ah * c / chunks, y1 = ah * (c + 1) / chunks;
            if (c == chunks - 1 && eye == 1) continue;  // the last chunk runs here
            jobs.push_back(std::async(std::launch::async, lumaRows, eye, y0, y1));
        }
    lumaRows(1, ah * (chunks - 1) / chunks, ah);
    for (auto& j : jobs) j.get();
    const double scale = p16 ? 1.0 / 256.0 : 1.0;
    for (int eye = 0; eye < 2; ++eye) {
        Sums t;
        for (auto& m : means[eye]) {
            const Sums s = m.get();
            t.y += s.y;
            t.u += s.u;
            t.v += s.v;
            t.n += s.n;
        }
        if (t.n > 0)
            YuvToRgb((float)((double)t.y * scale / (double)t.n), (float)((double)t.u * scale / (double)t.n),
                     (float)((double)t.v * scale / (double)t.n), f.fullRange, out.meanRgb[eye]);
    }
    return true;
}

namespace {

// Block-match `right` against templates cut from `left` (NCC, coarse then fine, sub-pixel
// peak, uniqueness against periodic texture). One entry per accepted block: its parallax
// dx, vertical offset dy (analysis px, right minus left) and template x; the normalised
// centre goes to r when given. With `mask`, only blocks whose centre is marked (want =
// true) or not marked (want = false) are matched.
void MatchBlocks(const std::vector<float>& left, const std::vector<float>& right, int W, int H, const Options& o,
                 const std::vector<uint8_t>* mask, bool want, std::vector<float>& dxs, std::vector<float>& dys,
                 std::vector<float>& xs, Result* r, std::vector<int>* ids = nullptr) {
    const int bs = o.block;
    const int R = std::max(2, (int)std::lround(o.maxParallaxPct / 100.f * W));
    const int D = o.maxDy;
    std::vector<float> tpl((size_t)bs * bs);
    for (int by = D + 2; by + bs + D + 2 <= H; by += o.step) {
        for (int bx = 2; bx + bs + 2 <= W; bx += o.step) {
            if (mask && ((*mask)[(size_t)(by + bs / 2) * W + bx + bs / 2] != 0) != want) continue;
            // template: centred left block
            float mean = 0;
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) mean += left[(size_t)(by + j) * W + bx + i];
            mean /= (float)(bs * bs);
            float norm = 0;
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) {
                    const float v = left[(size_t)(by + j) * W + bx + i] - mean;
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
                    const float c = Ncc(right, W, tpl.data(), norm, bs, bx + dx, by + dy);
                    if (c > best) {
                        best = c;
                        bdx = dx;
                        bdy = dy;
                    }
                }
            const int cx = bdx, cy = bdy;
            for (int dy = std::max(dyLo, cy - 2); dy <= std::min(dyHi, cy + 2); ++dy)
                for (int dx = std::max(dxLo, cx - 2); dx <= std::min(dxHi, cx + 2); ++dx) {
                    const float c = Ncc(right, W, tpl.data(), norm, bs, bx + dx, by + dy);
                    if (c > best) {
                        best = c;
                        bdx = dx;
                        bdy = dy;
                    }
                }
            if (best < o.minNcc) continue;
            // a peak on the edge of the search window is not a peak: the real match may
            // be outside, and such blocks read as the search limit itself (a +8 % "far"
            // alert from a 1.5 % scene, tmp-mp-pro 2026-10-18)
            if (bdx <= dxLo + 2 || bdx >= dxHi - 2 || bdy <= dyLo || bdy >= dyHi) continue;
            // uniqueness: no rival as good >= 4 px away along the row (periodic texture)
            float rival = -2.f;
            for (int dx = dxLo; dx <= dxHi; dx += 2)
                if (std::abs(dx - bdx) >= 4)
                    rival = std::max(rival, Ncc(right, W, tpl.data(), norm, bs, bx + dx, by + bdy));
            if (rival > best - 0.05f) continue;
            float fx = 0, fy = 0;
            if (bdx > dxLo && bdx < dxHi)
                fx = SubPixel(Ncc(right, W, tpl.data(), norm, bs, bx + bdx - 1, by + bdy), best,
                              Ncc(right, W, tpl.data(), norm, bs, bx + bdx + 1, by + bdy));
            if (bdy > dyLo && bdy < dyHi)
                fy = SubPixel(Ncc(right, W, tpl.data(), norm, bs, bx + bdx, by + bdy - 1), best,
                              Ncc(right, W, tpl.data(), norm, bs, bx + bdx, by + bdy + 1));
            dxs.push_back((float)bdx + fx);
            dys.push_back((float)bdy + fy);
            xs.push_back((float)bx + bs * 0.5f);
            if (ids) ids->push_back(by * W + bx);
            if (r) {
                r->matchX.push_back(((float)bx + bs * 0.5f) / (float)W);
                r->matchY.push_back(((float)by + bs * 0.5f) / (float)H);
            }
        }
    }
}

}  // namespace

Result Measure(const EyePair& e, const Options& o) {
    Result r;
    const int bs = o.block, W = e.eyeW, H = e.h;
    if (W < 4 * bs || H < 4 * bs) return r;
    std::vector<float> dxs, dys, xs;
    MatchBlocks(e.left, e.right, W, H, o, nullptr, true, dxs, dys, xs, &r);
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
    const double slopeA = sxx > 0 ? sxy / sxx : 0.0;  // analysis rows per analysis column
    const double slope = slopeA * (scaleY / (e.fullEyeW * e.xStretch / (float)W));
    r.rollDeg = (float)(std::atan(slope) * 180.0 / 3.14159265358979);
    r.dySlope = (float)slopeA;
    // vertical error: the offset at the picture centre with the roll taken out. A plain
    // median of dy reads the offset where the matches happen to sit, so a rolled eye
    // with its features off-centre inflated it (0.5 deg read as 5-6 px instead of 4).
    std::vector<float> dyc(dys.size());
    for (size_t i = 0; i < dys.size(); ++i) dyc[i] = dys[i] - (float)(slopeA * (xs[i] - 0.5 * W));
    r.verticalPx = median(dyc) * scaleY;
    {
        const float mc = median(dyc);
        std::vector<float> ad(dyc.size());
        for (size_t i = 0; i < dyc.size(); ++i) ad[i] = std::fabs(dyc[i] - mc);
        r.dyMad = median(ad) * scaleY;
    }
    r.deltaE = DeltaE76(e.meanRgb[0], e.meanRgb[1]);
    return r;
}

namespace {

// Mean absolute difference of two analysis-size luma images, b sampled at (x + sx, y + sy),
// over the overlap minus a margin, on the pixels `mask` marks (every 2nd pixel when null);
// each image's mean over those pixels is removed first so a gain / offset mismatch
// between the eyes does not count.
float Mad(const std::vector<float>& a, const std::vector<float>& b, int W, int H, int sx, int sy,
          const std::vector<uint8_t>* mask) {
    const int m = 4;
    const int x0 = std::max(m, m - sx), x1 = std::min(W - m, W - m - sx);
    const int y0 = std::max(m, m - sy), y1 = std::min(H - m, H - m - sy);
    if (x1 - x0 < 16 || y1 - y0 < 16) return -1.f;
    const int step = mask ? 1 : 2;  // the moving pixels are few: score all of them
    double ma = 0, mb = 0;
    int n = 0;
    for (int y = y0; y < y1; y += step)
        for (int x = x0; x < x1; x += step) {
            if (mask && !(*mask)[(size_t)y * W + x]) continue;
            ma += a[(size_t)y * W + x];
            mb += b[(size_t)(y + sy) * W + x + sx];
            ++n;
        }
    if (n < 64) return -1.f;
    ma /= n;
    mb /= n;
    double d = 0;
    for (int y = y0; y < y1; y += step)
        for (int x = x0; x < x1; x += step) {
            if (mask && !(*mask)[(size_t)y * W + x]) continue;
            d += std::fabs((a[(size_t)y * W + x] - ma) - (b[(size_t)(y + sy) * W + x + sx] - mb));
        }
    return (float)(d / n);
}

// img resampled so that out(x, y) = img(x, y + a + b * (x - W/2)) (bilinear, clamped):
// takes the rig's vertical offset line out of an eye, so a true match sits at dy = 0,
// an integer, where the sub-pixel peak is unbiased. (At a fractional offset the parabola
// peak locks toward the nearer integer by up to ~0.3 px, block by block, which is the
// size of a frame of motion at 48p and analysis size.)
std::vector<float> WarpY(const std::vector<float>& img, int W, int H, double a, double b) {
    std::vector<float> out((size_t)W * H);
    for (int x = 0; x < W; ++x) {
        const double off = a + b * (x - 0.5 * W);
        for (int y = 0; y < H; ++y) {
            const double sy = std::clamp((double)y + off, 0.0, (double)H - 1.0001);
            const int y0 = (int)sy;
            const float f = (float)(sy - y0);
            out[(size_t)y * W + x] = img[(size_t)y0 * W + x] * (1.f - f) + img[(size_t)(y0 + 1) * W + x] * f;
        }
    }
    return out;
}

// Where the left eye moved between two consecutive frames, grown by `grow` px so the
// same content in the right eye (at its parallax, a frame or two late) is inside too.
std::vector<uint8_t> MotionMask(const std::vector<float>& now, const std::vector<float>& before, int W, int H,
                                float thresh, int grow) {
    std::vector<uint8_t> raw((size_t)W * H, 0), out((size_t)W * H, 0);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = std::fabs(now[i] - before[i]) > thresh ? 1 : 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!raw[(size_t)y * W + x]) continue;
            for (int yy = std::max(0, y - grow); yy <= std::min(H - 1, y + grow); ++yy)
                for (int xx = std::max(0, x - grow); xx <= std::min(W - 1, x + grow); ++xx)
                    out[(size_t)yy * W + xx] = 1;
        }
    return out;
}

}  // namespace

int MeasureEyeSync(const std::vector<EyePair>& seq, float shiftX, float shiftY, float shiftSlope, int* lateEye) {
    (void)shiftX;
    if (lateEye) *lateEye = 0;
    const int n = (int)seq.size();
    if (n < 2) return -1;
    const EyePair& cur = seq.back();
    const int W = cur.eyeW, H = cur.h;
    for (const EyePair& e : seq)
        if (e.eyeW != W || e.h != H || e.left.size() != (size_t)W * H || e.right.size() != (size_t)W * H) return -1;
    // Motion: without it every candidate looks alike and there is nothing to measure.
    const float motion = Mad(cur.left, seq[n - 2].left, W, H, 0, 0, nullptr);
    if (motion < 0.4f) return -1;
    // What tells a late eye from parallax: a rig's vertical offset is one value over the
    // picture and over time, so content the matcher finds at a DIFFERENT vertical offset
    // in the moving parts is content from another instant. Horizontal offsets cannot do
    // it (a late eye of sideways motion looks exactly like depth), and a whole-picture
    // difference cannot either (the static background swamps it; it read a confident 0
    // or 3 for an injected 2-frame delay, tmp-mp-pro 2026-10-18).
    //
    // Two families, each with ONE template set so its candidates are scored on the same
    // blocks: the current right eye's moving blocks matched into the left eye now and k
    // frames back (a fit k back = the right eye shows old content = the right eye is k
    // late), and the current left eye's into the right eye now and k back (the left eye
    // is k late). Score = how far the blocks' vertical offsets sit from the rig's, taken
    // from the static blocks.
    const std::vector<uint8_t> mask = MotionMask(cur.left, seq[n - 2].left, W, H, 6.f, 3);
    Options o;
    o.block = 24;  // the sub-pixel vertical peak is what is scored: bigger blocks pin it better
    o.step = 12;
    const bool dbg = std::getenv("MEDIAPLAYER_RIG_DEBUG") != nullptr;
    auto median = [](std::vector<float> v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    // The rig's vertical offset as a LINE over x (a rolled eye's dy runs across the
    // picture: 0.5 deg is +-8 px full width, far more than a frame of motion), from the
    // spatial pass over the whole pair - not re-fitted on the static blocks here, which
    // can be few and untextured (a plain background) and then tilt the line at random.
    // Fitted here on the static blocks after an outlier cut (a flat or periodic
    // background matches at random dy; median +- k x MAD drops those), falling back to
    // the spatial pass's line (shiftY, shiftSlope) when too few static blocks survive.
    // The spatial pass alone is not good enough: its line includes the moving blocks,
    // whose dy is seen through the very delay being measured, and the bias that leaves
    // (up to a frame of motion, and a spurious 0.2 deg of slope) is the size of the
    // signal the candidates are told apart by.
    double rigA = shiftY, rigB = shiftSlope;  // dy(x) = rigA + rigB * (x - W/2)
    {
        std::vector<float> dxs, dys, xs;
        MatchBlocks(cur.left, cur.right, W, H, o, &mask, false, dxs, dys, xs, nullptr);
        if (dys.size() >= 8) {
            // de-roll with the spatial pass's slope for the cut only (a rolled eye's dy
            // spread is real), then fit on the survivors in raw dy
            std::vector<float> d(dys.size()), ad(dys.size());
            for (size_t i = 0; i < dys.size(); ++i) d[i] = dys[i] - (float)(shiftSlope * (xs[i] - 0.5 * W));
            const float md = median(d);
            for (size_t i = 0; i < d.size(); ++i) ad[i] = std::fabs(d[i] - md);
            const float cut = std::max(4.f * 1.4826f * median(ad), 1.0f);
            double mx = 0, my = 0;
            int m = 0;
            for (size_t i = 0; i < d.size(); ++i) {
                if (ad[i] > cut) continue;
                mx += xs[i];
                my += dys[i];
                ++m;
            }
            if (m >= 8) {
                mx /= m;
                my /= m;
                double sxy = 0, sxx = 0;
                for (size_t i = 0; i < d.size(); ++i) {
                    if (ad[i] > cut) continue;
                    sxy += (xs[i] - mx) * (dys[i] - my);
                    sxx += (xs[i] - mx) * (xs[i] - mx);
                }
                rigB = sxx > 0 ? sxy / sxx : 0.0;
                rigA = my + rigB * (0.5 * W - mx);
            }
            if (dbg) std::fprintf(stderr, "    eyesync rig line from %d/%zu static blocks\n", m, dys.size());
        }
    }
    // The rig's line is taken OUT of the search eyes by a vertical warp, so a true match
    // sits at dy = 0 for every block and family alike; the line itself is then refined
    // once on the warped static blocks (near 0, unbiased) before the candidates run.
    // right(x, y) ~ left(x, y - rig(x)): the left eye warped by -rig lines up with the
    // right eye, the right eye warped by +rig with the left.
    auto refine = [&]() {
        std::vector<float> dxs, dys, xs;
        const std::vector<float> rw = WarpY(cur.right, W, H, rigA, rigB);
        MatchBlocks(cur.left, rw, W, H, o, &mask, false, dxs, dys, xs, nullptr);
        if (dys.size() < 8) return;
        std::vector<float> ad(dys.size());
        const float md = median(dys);
        for (size_t i = 0; i < dys.size(); ++i) ad[i] = std::fabs(dys[i] - md);
        const float cut = std::max(4.f * 1.4826f * median(ad), 0.5f);
        double mx = 0, my = 0;
        int m = 0;
        for (size_t i = 0; i < dys.size(); ++i) {
            if (ad[i] > cut) continue;
            mx += xs[i];
            my += dys[i];
            ++m;
        }
        if (m < 8) return;
        mx /= m;
        my /= m;
        double sxy = 0, sxx = 0;
        for (size_t i = 0; i < dys.size(); ++i) {
            if (ad[i] > cut) continue;
            sxy += (xs[i] - mx) * (dys[i] - my);
            sxx += (xs[i] - mx) * (xs[i] - mx);
        }
        const double db = sxx > 0 ? sxy / sxx : 0.0;
        const double da = my + db * (0.5 * W - mx);
        rigA += da;
        rigB += db;
    };
    refine();
    if (dbg) std::fprintf(stderr, "    eyesync rig line: dy = %.3f %+.5f/px * (x - W/2)\n", rigA, rigB);
    struct Cand {
        float score;
        int frames, late;
    };
    std::vector<Cand> c;
    // One family: template eye `tpl` (of the current frame), searched in the other eye of
    // the frame k back (warped onto the template eye's geometry), k = 0..n-1.
    // Each family's moving blocks come from ITS template eye's own motion: a late eye's
    // moving region sits a few px from the other eye's, and a block that is static in
    // the template matches every candidate alike and drags the in-step score down.
    // The warp (a bilinear resample) is used only when the roll matters - a slope worth
    // over half a pixel at the picture's edges. Resampling softens the searched eye and
    // that costs sub-pixel precision; without roll the offset is scored directly
    // (the no-roll case lost a third of its correct readings to the warp, tmp-mp-pro).
    const bool rolled = std::fabs(rigB) * 0.5 * W > 0.5;
    if (dbg) std::fprintf(stderr, "    eyesync %s\n", rolled ? "warping the roll out" : "no roll: direct offset");
    auto family = [&](const std::vector<float>& tpl, const std::vector<float>& tplBefore, auto searchEye, int late,
                      double wa, double wb) {
        const std::vector<uint8_t> tmask = MotionMask(tpl, tplBefore, W, H, 6.f, 3);
        std::vector<std::vector<float>> dy(n), x(n);
        std::vector<std::vector<int>> id(n);
        // The candidates are independent block-match passes over const images: one task
        // each. In series they were ~2/3 of a ~0.5 s measurement on a laptop CPU (Intel
        // PTL, Linux), which held the worker to ~2 bursts a second instead of ~4.
        auto candidate = [&](int k) {
            std::vector<float> dxs;
            if (rolled) {
                const std::vector<float> warped = WarpY(searchEye(seq[n - 1 - k]), W, H, wa, wb);
                MatchBlocks(tpl, warped, W, H, o, &tmask, true, dxs, dy[k], x[k], nullptr, &id[k]);
            } else {
                MatchBlocks(tpl, searchEye(seq[n - 1 - k]), W, H, o, &tmask, true, dxs, dy[k], x[k], nullptr, &id[k]);
            }
        };
        std::vector<std::future<void>> tasks;
        for (int k = 1; k < n; ++k) tasks.push_back(std::async(std::launch::async, candidate, k));
        candidate(0);
        for (auto& t : tasks) t.get();
        const float expect = rolled ? 0.f : (float)wa;  // the warp moves the search by wa, so unwarped the match sits at wa
        // the blocks every candidate matched
        std::vector<int> common = id[0];
        std::sort(common.begin(), common.end());
        for (int k = 1; k < n; ++k) {
            std::vector<int> s = id[k], out;
            std::sort(s.begin(), s.end());
            std::set_intersection(common.begin(), common.end(), s.begin(), s.end(), std::back_inserter(out));
            common.swap(out);
        }
        if (dbg) std::fprintf(stderr, "    eyesync family late=%+d: %zu common moving blocks\n", late, common.size());
        if (common.size() < 6) return;
        for (int k = 0; k < n; ++k) {
            std::vector<float> v;
            for (size_t i = 0; i < id[k].size(); ++i)
                if (std::binary_search(common.begin(), common.end(), id[k][i])) v.push_back(std::fabs(dy[k][i] - expect));
            c.push_back({median(v), k, k == 0 ? 0 : late});
            if (dbg) std::fprintf(stderr, "      k=%d: score %.3f\n", k, c.back().score);
        }
    };
    // the right eye late: its content is k frames old, so it lines up with the LEFT eye
    // k frames back (warped by -rig onto the right eye's geometry)
    family(cur.right, seq[n - 2].right, [](const EyePair& e) -> const std::vector<float>& { return e.left; }, +1, -rigA, -rigB);
    // the left eye late: lines up with the RIGHT eye k frames back (warped by +rig)
    family(cur.left, seq[n - 2].left, [](const EyePair& e) -> const std::vector<float>& { return e.right; }, -1, rigA, rigB);
    if (c.size() < 2) return -1;
    std::sort(c.begin(), c.end(), [](const Cand& x, const Cand& y) { return x.score < y.score; });
    // A clear winner only: the runner-up must be clearly worse, by more than the
    // matcher's sub-pixel noise. The in-step candidate appears in both families; the
    // runner-up is the best candidate that is not the same reading.
    size_t ru = 1;
    while (ru < c.size() && c[ru].frames == c[0].frames && c[ru].late == c[0].late) ++ru;
    if (ru >= c.size()) return -1;
    // (0.12 / 25 % before the analysis image was box-filtered: the aliased image's
    // sub-pixel noise needed it; on the filtered image it threw away most correct
    // readings at 48p, and the vote across bursts in SyncConfirm guards against a
    // stray one.)
    if (c[ru].score - c[0].score < std::max(0.06f, 0.2f * c[ru].score)) return -1;
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
    e.serial = f.serial;
    const auto dt0 = std::chrono::steady_clock::now();
    const bool downOk = Downsample(f, layout, 640, e, swapEyes);
    {
        // Render-thread cost of the downsample, logged every 5 s (measurement aid).
        static double sum = 0.0, mx = 0.0, nextLog = 0.0;
        static int cnt = 0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - dt0).count();
        sum += ms;
        mx = std::max(mx, ms);
        ++cnt;
        if (now >= nextLog) {
            if (nextLog > 0.0)
                std::fprintf(stderr, "[mediaplayer][INFO ] Rig check (render thread): downsample %d x avg %.2f max %.2f ms\n",
                             cnt, sum / cnt, mx);
            sum = mx = 0.0;
            cnt = 0;
            nextLog = now + 5.0;
        }
    }
    if (!downOk) {
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
    pendingLayout_ = layout;
    pendingSwap_ = swapEyes;
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
        const StereoLayout layout = pendingLayout_;
        const bool swap = pendingSwap_;
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
            const int m = MeasureEyeSync(seq, shiftX, shiftY, r.dySlope, &r.eyeSyncLateEye);
            if (gen != syncGen_) {  // Reset() ran since the last burst: start the vote afresh
                syncConfirm_ = SyncConfirm{};
                syncGen_ = gen;
            }
            r.eyeSyncMeasuredFrames = syncConfirm_.Feed(m, r.eyeSyncLateEye, LiveProducer::NowSeconds(), &r.eyeSyncLateEye);
            // A late eye: the rig's geometry is what the eyes show at the SAME instant, so
            // re-measure on the time-aligned pair (the late eye now, the other eye that many
            // frames back). Otherwise moving content read through the delay shifts the
            // vertical error and the depth range with every frame. The picture reading says
            // which eye; the timecode figure (used when the picture has none) does not, so
            // both alignments are tried and the one that matches better is the true one -
            // a true alignment matches at least as well as the raw pair, a wrong one worse.
            int d = r.eyeSyncMeasuredFrames, lateEye = r.eyeSyncLateEye;
            if (d <= 0 && eyeOffset > 0) {
                d = eyeOffset;
                lateEye = 0;
            }
            if (d > 0 && d < (int)seq.size()) {
                const EyePair& back = seq[seq.size() - 1 - (size_t)d];
                // the true alignment is the one whose vertical residuals are tightest: the
                // other eye's moving content then sits on the rig's line, while a wrong
                // alignment (or none) scatters it (a 1-7 px vertical and a far value pinned
                // near the search limit came from match counts picking the wrong side)
                Result best;
                for (int side = -1; side <= 1; side += 2) {
                    if (lateEye != 0 && side != lateEye) continue;
                    EyePair aligned = seq.back();
                    if (side > 0) aligned.left = back.left;   // right eye late: left eye d back
                    else aligned.right = back.right;          // left eye late: right eye d back
                    Result a = Measure(aligned);
                    if (!a.ok || a.matches < (r.matches * 7) / 10) continue;
                    if (!best.ok || a.dyMad < best.dyMad) best = std::move(a);
                }
                if (best.ok && best.dyMad <= r.dyMad + 0.05f) {
                    best.eyeSyncMeasuredFrames = r.eyeSyncMeasuredFrames;
                    best.eyeSyncLateEye = r.eyeSyncLateEye;
                    r = std::move(best);
                }
            }
            if (std::getenv("MEDIAPLAYER_RIG_DEBUG")) {
                const EyePair& c = seq.back();
                std::fprintf(stderr,
                             "    rig: vertical %.2f px (mad %.2f) roll %.2f deg dE %.1f near %.2f far %.2f matches %d "
                             "(aligned d=%d) serial %lld layout %d swap %d meanG %.3f/%.3f\n",
                             r.verticalPx, r.dyMad, r.rollDeg, r.deltaE, r.nearPct, r.farPct, r.matches, d,
                             (long long)c.serial, (int)layout, (int)swap, c.meanRgb[0][1], c.meanRgb[1][1]);
            }
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
