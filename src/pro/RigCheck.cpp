// SPDX-License-Identifier: Apache-2.0
#include "pro/RigCheck.h"

#include "media/LiveProducer.h"
#include "pro/TargetScreen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <iterator>
#include <limits>
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

namespace {

// Where the two eyes sit in a packed frame (side by side or top/bottom), in source pixels,
// as SHOWN: with the eyes swapped the second half feeds the left eye. Half layouts are
// squeezed along their packing axis and stretched back on display (x/yStretch).
struct EyeGeom {
    int x0[2] = {}, y0[2] = {};
    int w = 0, h = 0;  // one eye as stored
    float xStretch = 1.f, yStretch = 1.f;
};

bool EyeGeometry(StereoLayout layout, int frameW, int frameH, bool swapEyes, EyeGeom& g) {
    const bool sbs = layout == StereoLayout::SbsFull || layout == StereoLayout::SbsHalf;
    const bool tb = layout == StereoLayout::TbFull || layout == StereoLayout::TbHalf;
    if (!sbs && !tb) return false;
    g.w = sbs ? frameW / 2 : frameW;
    g.h = tb ? frameH / 2 : frameH;
    for (int eye = 0; eye < 2; ++eye) {
        const int k = eye ^ (swapEyes ? 1 : 0);
        g.x0[eye] = sbs ? k * g.w : 0;
        g.y0[eye] = tb ? k * g.h : 0;
    }
    g.xStretch = layout == StereoLayout::SbsHalf ? 2.f : 1.f;
    g.yStretch = layout == StereoLayout::TbHalf ? 2.f : 1.f;
    return g.w >= 32 && g.h >= 32;
}

// Integer sums of one eye's mean-colour samples (three channels: Y'CbCr or R'G'B').
struct Sums {
    uint64_t c[3] = {};
    int64_t n = 0;
};

// The downsample shared by every pixel format: a box filter of each eye to the analysis
// size and each eye's mean colour, on several threads. The format lives in the callbacks:
//   addRow(eye, row, colSum)       add source row `row` (frame coordinates) of that eye's
//                                  columns into colSum (g.w integer sums of luma)
//   meanRow(eye, row, x0, x1, s)   add that eye's colour samples of columns [x0, x1) of
//                                  eye-local row `row` into s
//   toRgb(sums, rgb)               the mean (sums / sums.n) as display-referred R'G'B' 0..1
// unit scales the luma sums to 0..255. Each analysis pixel and each mean is computed
// exactly as on one thread (each output row is independent; each mean is summed on one
// thread, and integer sums do not depend on their order).
template <class AddRow, class MeanRow, class ToRgb>
void DownsampleEyes(const EyeGeom& g, int analysisW, float unit, AddRow addRow, MeanRow meanRow, ToRgb toRgb,
                    EyePair& out) {
    const int aw = std::min(analysisW, g.w);
    const float sx = (float)g.w / (float)aw;
    // A squeezed top/bottom eye gets its displayed rows back (yStretch), so the vertical
    // error keeps the precision of a full-height eye.
    const int ah = std::max(16, (int)std::lround(g.h * g.yStretch / sx));
    const float sy = (float)g.h / (float)ah;
    out.eyeW = aw;
    out.h = ah;
    out.fullEyeW = (float)g.w;
    out.fullH = (float)g.h;
    out.xStretch = g.xStretch;
    out.yStretch = g.yStretch;
    out.left.resize((size_t)aw * ah);
    out.right.resize((size_t)aw * ah);
    // At 7680x2160 SBS this was 8-9 ms a call (max ~24) on the render thread, ~9 calls a
    // second during bursts (#120 review, 2026-10-19), hence the threads.
    auto lumaRows = [&](int eye, int yBegin, int yEnd) {
        std::vector<float>& dst = eye == 0 ? out.left : out.right;
        // Integer sums: the samples are integers (scaled by `unit` at the end), every
        // partial sum stays under 2^24, so the result is the float sum bit for bit.
        std::vector<uint32_t> colSum((size_t)g.w);
        // A box over each analysis pixel's whole footprint (sx x sy source pixels). Sampling
        // a 2x2 corner of a 3x3 footprint, as this did, skips five source pixels in nine:
        // an edge moving through the skipped rows does not move in the analysis image,
        // so sub-pixel motion is quantised and a frame of motion at 48p (~0.3 px here)
        // vanished - the eye-sync reading split between the true delay and its neighbour
        // or read "in step" (tmp-mp-pro, 2026-10-18).
        for (int y = yBegin; y < yEnd; ++y) {
            const int ya = std::min(g.h - 1, (int)(y * sy));
            const int yb = std::max(ya + 1, std::min(g.h, (int)((y + 1) * sy)));
            std::fill(colSum.begin(), colSum.end(), 0u);
            for (int yy = ya; yy < yb; ++yy) addRow(eye, g.y0[eye] + yy, colSum.data());
            float* d = dst.data() + (size_t)y * aw;
            for (int x = 0; x < aw; ++x) {
                const int xa = std::min(g.w - 1, (int)(x * sx));
                const int xb = std::max(xa + 1, std::min(g.w, (int)((x + 1) * sx)));
                uint32_t acc = 0;
                for (int xx = xa; xx < xb; ++xx) acc += colSum[xx];
                d[x] = ((float)acc * unit) / (float)((xb - xa) * (yb - ya));
            }
        }
    };
    // Mean colour over the central 80 %: whole rows, every 7th. A sparse grid aliases with
    // periodic content (a 16 px grid on a drifting 160 px grating read one eye 15 % brighter
    // for a burst now and then, and the colour alert latched that peak - tmp-mp-pro
    // 2026-10-18). Summed in integers per chunk of rows, so the total is exact.
    const int rowFirst = g.h / 10, rowEnd = g.h * 9 / 10;
    const int rowCount = rowFirst < rowEnd ? (rowEnd - rowFirst + 6) / 7 : 0;
    const int xFirst = g.w / 10, xEnd = g.w * 9 / 10;
    auto meanRows = [&](int eye, int kBegin, int kEnd) {
        Sums s;
        for (int k = kBegin; k < kEnd; ++k) {
            meanRow(eye, rowFirst + 7 * k, xFirst, xEnd, s);
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
    for (int eye = 0; eye < 2; ++eye) {
        Sums t;
        for (auto& m : means[eye]) {
            const Sums s = m.get();
            for (int k = 0; k < 3; ++k) t.c[k] += s.c[k];
            t.n += s.n;
        }
        if (t.n > 0) toRgb(t, out.meanRgb[eye]);
    }
}

}  // namespace

bool Downsample(const FrameRing::Frame& f, StereoLayout layout, int analysisW, EyePair& out, bool swapEyes) {
    if (f.gpu || f.plane[0].empty()) return false;
    if (f.format != PixFormat::NV12 && f.format != PixFormat::I420 && f.format != PixFormat::P016) return false;
    EyeGeom g;
    if (!EyeGeometry(layout, f.width, f.height, swapEyes, g)) return false;
    const bool p16 = f.format == PixFormat::P016;  // 16-bit samples, MSB-aligned
    const int W = f.width, cw = W / 2;
    auto addRow = [&](int eye, int row, uint32_t* colSum) {
        const size_t r = (size_t)row * W + g.x0[eye];
        if (p16) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(f.plane[0].data()) + r;
            for (int x = 0; x < g.w; ++x) colSum[x] += src[x];
        } else {
            const uint8_t* src = f.plane[0].data() + r;
            for (int x = 0; x < g.w; ++x) colSum[x] += src[x];
        }
    };
    auto meanRow = [&](int eye, int yEye, int xFirst, int xEnd, Sums& s) {
        const int y = g.y0[eye] + yEye, x0 = g.x0[eye];
        if (p16) {
            const uint16_t* Y = reinterpret_cast<const uint16_t*>(f.plane[0].data()) + (size_t)y * W;
            const uint16_t* UV = reinterpret_cast<const uint16_t*>(f.plane[1].data()) + (size_t)(y / 2) * W;
            for (int x = xFirst; x < xEnd; ++x) {
                const int gx = x0 + x;
                s.c[0] += Y[gx];
                s.c[1] += UV[gx & ~1];
                s.c[2] += UV[(gx & ~1) + 1];
            }
        } else if (f.format == PixFormat::NV12) {
            const uint8_t* Y = f.plane[0].data() + (size_t)y * W;
            const uint8_t* UV = f.plane[1].data() + (size_t)(y / 2) * W;
            for (int x = xFirst; x < xEnd; ++x) {
                const int gx = x0 + x;
                s.c[0] += Y[gx];
                s.c[1] += UV[gx & ~1];
                s.c[2] += UV[(gx & ~1) + 1];
            }
        } else {
            const uint8_t* Y = f.plane[0].data() + (size_t)y * W;
            const uint8_t* U = f.plane[1].data() + (size_t)(y / 2) * cw;
            const uint8_t* V = f.plane[2].data() + (size_t)(y / 2) * cw;
            for (int x = xFirst; x < xEnd; ++x) {
                const int gx = x0 + x;
                s.c[0] += Y[gx];
                s.c[1] += U[gx / 2];
                s.c[2] += V[gx / 2];
            }
        }
    };
    const double scale = p16 ? 1.0 / 256.0 : 1.0;
    auto toRgb = [&](const Sums& t, float rgb[3]) {
        const double n = (double)t.n;
        YuvToRgb((float)((double)t.c[0] * scale / n), (float)((double)t.c[1] * scale / n),
                 (float)((double)t.c[2] * scale / n), f.fullRange, rgb);
    };
    DownsampleEyes(g, analysisW, p16 ? 1.0f / 256.0f : 1.0f, addRow, meanRow, toRgb, out);
    return true;
}

bool DownsampleRgba(const uint8_t* rgba, int w, int h, StereoLayout layout, int analysisW, EyePair& out,
                    bool swapEyes) {
    if (!rgba) return false;
    EyeGeom g;
    if (!EyeGeometry(layout, w, h, swapEyes, g)) return false;
    // Luma: BT.709 Y' from the (full-range) R'G'B', weights in 1/256 so the sums stay integer.
    auto addRow = [&](int eye, int row, uint32_t* colSum) {
        const uint8_t* src = rgba + ((size_t)row * w + g.x0[eye]) * 4;
        for (int x = 0; x < g.w; ++x, src += 4) colSum[x] += 54u * src[0] + 183u * src[1] + 19u * src[2];
    };
    auto meanRow = [&](int eye, int yEye, int xFirst, int xEnd, Sums& s) {
        const uint8_t* src = rgba + ((size_t)(g.y0[eye] + yEye) * w + g.x0[eye] + xFirst) * 4;
        for (int x = xFirst; x < xEnd; ++x, src += 4) {
            s.c[0] += src[0];
            s.c[1] += src[1];
            s.c[2] += src[2];
        }
    };
    auto toRgb = [](const Sums& t, float rgb[3]) {
        for (int k = 0; k < 3; ++k) rgb[k] = (float)((double)t.c[k] / 255.0 / (double)t.n);
    };
    DownsampleEyes(g, analysisW, 1.0f / 256.0f, addRow, meanRow, toRgb, out);
    return true;
}

namespace {

// One block: the template at (bx, by) of `a`, searched for in `b` over dx in [-R, R] and
// dy in [-D, D], clipped to the frame (NCC, coarse then fine, sub-pixel peak, uniqueness
// against periodic texture). On success dx / dy are where it sits in `b` minus where it
// sits in `a`, analysis px. tpl is scratch (bs * bs).
bool MatchAt(const std::vector<float>& a, const std::vector<float>& b, int W, int H, const Options& o, int bx, int by,
             int R, int D, std::vector<float>& tpl, float& outDx, float& outDy, float* outScore = nullptr) {
    const int bs = o.block;
    float mean = 0;
    for (int j = 0; j < bs; ++j)
        for (int i = 0; i < bs; ++i) mean += a[(size_t)(by + j) * W + bx + i];
    mean /= (float)(bs * bs);
    float norm = 0;
    for (int j = 0; j < bs; ++j)
        for (int i = 0; i < bs; ++i) {
            const float v = a[(size_t)(by + j) * W + bx + i] - mean;
            tpl[(size_t)j * bs + i] = v;
            norm += v * v;
        }
    const float sd = std::sqrt(norm / (float)(bs * bs));
    if (sd < o.minTexture) return false;
    norm = std::sqrt(norm);
    const int dxLo = std::max(-R, -bx), dxHi = std::min(R, W - bs - bx);
    const int dyLo = std::max(-D, -by), dyHi = std::min(D, H - bs - by);
    // coarse (step 2), then refine (+-2, step 1)
    float best = -2.f;
    int bdx = 0, bdy = 0;
    for (int dy = dyLo; dy <= dyHi; dy += 2)
        for (int dx = dxLo; dx <= dxHi; dx += 2) {
            const float c = Ncc(b, W, tpl.data(), norm, bs, bx + dx, by + dy);
            if (c > best) {
                best = c;
                bdx = dx;
                bdy = dy;
            }
        }
    const int cx = bdx, cy = bdy;
    for (int dy = std::max(dyLo, cy - 2); dy <= std::min(dyHi, cy + 2); ++dy)
        for (int dx = std::max(dxLo, cx - 2); dx <= std::min(dxHi, cx + 2); ++dx) {
            const float c = Ncc(b, W, tpl.data(), norm, bs, bx + dx, by + dy);
            if (c > best) {
                best = c;
                bdx = dx;
                bdy = dy;
            }
        }
    if (best < o.minNcc) return false;
    // a peak on the edge of the search window is not a peak: the real match may
    // be outside, and such blocks read as the search limit itself (a +8 % "far"
    // alert from a 1.5 % scene, tmp-mp-pro 2026-10-18)
    if (bdx <= dxLo + 2 || bdx >= dxHi - 2 || bdy <= dyLo || bdy >= dyHi) return false;
    // uniqueness: no rival as good >= 4 px away along the row (periodic texture)
    float rival = -2.f;
    for (int dx = dxLo; dx <= dxHi; dx += 2)
        if (std::abs(dx - bdx) >= 4) rival = std::max(rival, Ncc(b, W, tpl.data(), norm, bs, bx + dx, by + bdy));
    if (rival > best - 0.05f) return false;
    float fx = 0, fy = 0;
    if (bdx > dxLo && bdx < dxHi)
        fx = SubPixel(Ncc(b, W, tpl.data(), norm, bs, bx + bdx - 1, by + bdy), best,
                      Ncc(b, W, tpl.data(), norm, bs, bx + bdx + 1, by + bdy));
    if (bdy > dyLo && bdy < dyHi)
        fy = SubPixel(Ncc(b, W, tpl.data(), norm, bs, bx + bdx, by + bdy - 1), best,
                      Ncc(b, W, tpl.data(), norm, bs, bx + bdx, by + bdy + 1));
    outDx = (float)bdx + fx;
    outDy = (float)bdy + fy;
    if (outScore) *outScore = best;
    return true;
}

// Block-match `right` against templates cut from `left` (MatchAt). One entry per accepted
// block: its parallax dx, vertical offset dy (analysis px, right minus left) and template
// x; the normalised centre goes to r when given. With `mask`, only blocks whose centre is
// marked (want = true) or not marked (want = false) are matched.
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
            float dx, dy;
            if (!MatchAt(left, right, W, H, o, bx, by, R, D, tpl, dx, dy)) continue;
            dxs.push_back(dx);
            dys.push_back(dy);
            xs.push_back((float)bx + bs * 0.5f);
            if (ids) ids->push_back(by * W + bx);
            if (r) {
                r->matchX.push_back(((float)bx + bs * 0.5f) / (float)W);
                r->matchY.push_back(((float)by + bs * 0.5f) / (float)H);
            }
        }
    }
}

// Edge (window) violations: blocks in a band along each frame edge, matched from the eye
// whose image of a foreground object is nearer that edge (EdgeSide), on a denser column
// step than the main grid so a narrow band still has several columns.
void MeasureEdges(const EyePair& e, const Options& o, Result& r) {
    const int bs = o.block, W = e.eyeW, H = e.h;
    const int R = std::max(2, (int)std::lround(o.maxParallaxPct / 100.f * W));
    const int D = o.maxDy;
    const int band = std::max(bs + 4, (int)std::lround(o.edgeBandPct / 100.f * W));
    const int xStep = std::max(2, bs / 2);
    std::vector<float> tpl((size_t)bs * bs);
    for (int side = 0; side < 2; ++side) {
        EdgeSide& es = r.edge[side];
        // left edge: right-eye blocks found in the left eye (parallax = right - left = -dx);
        // right edge: left-eye blocks found in the right eye (parallax = dx)
        const std::vector<float>& a = side == 0 ? e.right : e.left;
        const std::vector<float>& b = side == 0 ? e.left : e.right;
        for (int by = D + 2; by + bs + D + 2 <= H; by += o.step) {
            for (int k = 0;; ++k) {
                const int bx = side == 0 ? 2 + k * xStep : W - bs - 2 - k * xStep;
                const int inner = side == 0 ? bx + bs : W - bx;  // how far in the block reaches
                if (inner > band || bx < 2 || bx + bs + 2 > W) break;
                float dx, dy;
                if (!MatchAt(a, b, W, H, o, bx, by, R, D, tpl, dx, dy)) continue;
                es.matchY.push_back(((float)by + bs * 0.5f) / (float)H);
                es.parallax.push_back((side == 0 ? -dx : dx) / (float)W * 100.f);
            }
        }
        es.measured = (int)es.parallax.size() >= std::max(1, o.edgeMinMatches);
        if (es.measured) {
            // the 2nd most in front: one stray block does not make a violation
            std::vector<float> v = es.parallax;
            std::sort(v.begin(), v.end());
            es.parallaxPct = v[std::min<size_t>(1, v.size() - 1)];
        }
    }
}

}  // namespace

Result Measure(const EyePair& e, const Options& o) {
    Result r;
    const int bs = o.block, W = e.eyeW, H = e.h;
    if (W < 4 * bs || H < 4 * bs) return r;
    MeasureEdges(e, o, r);
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
    r.medianPct = pct(0.5f);
    r.pxPerPct = e.fullEyeW * e.xStretch / 100.f;
    const float scaleY = e.PxPerRow();
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
    const double slope = slopeA * (scaleY / e.PxPerCol());
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

std::vector<int> Histogram(const std::vector<float>& pct, float lo, float hi, int bins) {
    std::vector<int> h((size_t)std::max(1, bins), 0);
    if (!(hi > lo)) return h;
    const float k = (float)h.size() / (hi - lo);
    for (float p : pct) h[(size_t)std::clamp((int)std::floor((p - lo) * k), 0, (int)h.size() - 1)]++;
    return h;
}

DenseMap MeasureDense(const EyePair& e) {
    DenseMap m;
    const auto t0 = std::chrono::steady_clock::now();
    const int W = e.eyeW / 2, H = e.h / 2;
    if (W < 64 || H < 32 || e.left.size() < (size_t)e.eyeW * e.h || e.right.size() < e.left.size()) return m;
    // a 2x box-down of the analysis eyes: 320 px per eye, about 40 x 20 cells
    std::vector<float> L((size_t)W * H), R((size_t)W * H);
    for (int y = 0; y < H; ++y) {
        const size_t a = (size_t)(2 * y) * e.eyeW, b = a + e.eyeW;
        for (int x = 0; x < W; ++x) {
            L[(size_t)y * W + x] = 0.25f * (e.left[a + 2 * x] + e.left[a + 2 * x + 1] + e.left[b + 2 * x] + e.left[b + 2 * x + 1]);
            R[(size_t)y * W + x] =
                0.25f * (e.right[a + 2 * x] + e.right[a + 2 * x + 1] + e.right[b + 2 * x] + e.right[b + 2 * x + 1]);
        }
    }
    Options o;
    o.block = 12;
    o.step = 8;
    o.maxDy = 3;  // rows at 320 px: a rig within ~12 px vertical at 1080p
    const int bs = o.block, D = o.maxDy;
    const int Rx = std::max(2, (int)std::lround(o.maxParallaxPct / 100.f * W));
    const int bx0 = 2, by0 = D + 2;
    m.cols = (W - bs - 2 - bx0) / o.step + 1;
    m.rows = (H - bs - D - 2 - by0) / o.step + 1;
    if (m.cols < 1 || m.rows < 1) return DenseMap{};
    m.u0 = ((float)bx0 + bs * 0.5f) / (float)W;
    m.v0 = ((float)by0 + bs * 0.5f) / (float)H;
    m.du = (float)o.step / (float)W;
    m.dv = (float)o.step / (float)H;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> raw((size_t)m.cols * m.rows, nan);
    // ~800 blocks: split by rows over a few threads, so a pass stays a few ms (it runs on
    // the rig worker, never the render thread)
    auto rows = [&](int j0, int j1) {
        std::vector<float> tpl((size_t)bs * bs);
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < m.cols; ++i) {
                float dx, dy;
                if (MatchAt(L, R, W, H, o, bx0 + i * o.step, by0 + j * o.step, Rx, D, tpl, dx, dy))
                    raw[(size_t)j * m.cols + i] = dx / (float)W * 100.f;
            }
    };
    const int chunks = std::clamp((int)std::thread::hardware_concurrency() / 2, 1, 4);
    std::vector<std::future<void>> jobs;
    for (int c = 1; c < chunks; ++c)
        jobs.push_back(std::async(std::launch::async, rows, m.rows * c / chunks, m.rows * (c + 1) / chunks));
    rows(0, m.rows / chunks);
    for (auto& j : jobs) j.get();
    // MAD cut, as Measure's: a block locked on the wrong repeat of a texture is one wild value
    std::vector<float> v;
    for (float p : raw)
        if (!std::isnan(p)) v.push_back(p);
    if (v.size() >= 8) {
        auto median = [](std::vector<float> a) {
            std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end());
            return a[a.size() / 2];
        };
        const float med = median(v);
        std::vector<float> ad(v.size());
        for (size_t k = 0; k < v.size(); ++k) ad[k] = std::fabs(v[k] - med);
        const float t = std::max(5.f * 1.4826f * median(ad), 3.f);  // the scene's depth spread must survive
        for (float& p : raw)
            if (!std::isnan(p) && std::fabs(p - med) > t) p = nan;
    }
    // 3x3 median hole fill: an empty cell with at least half of its neighbours measured
    // takes their median; wide flat areas stay empty (transparent on the heatmap)
    m.pct = raw;
    for (int j = 0; j < m.rows; ++j)
        for (int i = 0; i < m.cols; ++i) {
            const size_t c = (size_t)j * m.cols + i;
            if (!std::isnan(raw[c])) {
                ++m.matched;
                continue;
            }
            float nb[8];
            int n = 0;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const int ii = i + di, jj = j + dj;
                    if ((di || dj) && ii >= 0 && jj >= 0 && ii < m.cols && jj < m.rows &&
                        !std::isnan(raw[(size_t)jj * m.cols + ii]))
                        nb[n++] = raw[(size_t)jj * m.cols + ii];
                }
            if (n < 4) continue;
            std::nth_element(nb, nb + n / 2, nb + n);
            m.pct[c] = nb[n / 2];
            ++m.filled;
        }
    m.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return m;
}

Probe ProbeAt(const EyePair& e, float u, float v, float& pct, float& conf, const Options& o) {
    pct = conf = 0.f;
    const int W = e.eyeW, H = e.h, D = o.maxDy;
    if (W < 8 * o.block || H < 8 * o.block || e.left.size() < (size_t)W * H || u < 0.f || u > 1.f || v < 0.f ||
        v > 1.f)
        return Probe::None;
    const int R = std::max(2, (int)std::lround(o.maxParallaxPct / 100.f * W));
    // The grid's block first; on a smooth area (the middle of a large shaded object) a
    // block twice as wide, so the point still reads when its surroundings carry texture.
    Probe got = Probe::Flat;
    for (int bs = o.block; bs <= 2 * o.block; bs *= 2) {
        Options ob = o;
        ob.block = bs;
        // the block centred on the point, kept where the search window fits (as the grid's)
        const int bx = std::clamp((int)std::lround(u * W - bs * 0.5f), 2, W - bs - 2);
        const int by = std::clamp((int)std::lround(v * H - bs * 0.5f), D + 2, H - bs - D - 2);
        float mean = 0, sq = 0;
        for (int j = 0; j < bs; ++j)
            for (int i = 0; i < bs; ++i) {
                const float x = e.left[(size_t)(by + j) * W + bx + i];
                mean += x;
                sq += x * x;
            }
        const float n = (float)(bs * bs);
        mean /= n;
        if (std::sqrt(std::max(0.f, sq / n - mean * mean)) < o.minTexture) continue;
        std::vector<float> tpl((size_t)bs * bs);
        float dx, dy;
        if (MatchAt(e.left, e.right, W, H, ob, bx, by, R, D, tpl, dx, dy, &conf)) {
            pct = dx / (float)W * 100.f;
            return Probe::Ok;
        }
        got = Probe::NoMatch;
    }
    return got;
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
    {
        std::lock_guard<std::mutex> pl(pairM_);
        probeStop_ = true;
    }
    probeCv_.notify_all();
    if (probeThread_.joinable()) probeThread_.join();
}

void RigMonitor::Submit(const FrameRing::Frame& f, StereoLayout layout, int eyeOffsetFrames,
                        const std::string& timecode, bool swapEyes) {
    const double now = LiveProducer::NowSeconds();
    if (stillHeld_) ClearStill();  // frames again
    // Frames the downsample cannot read say why, instead of a "Measuring..." that never
    // ends: zero-copy decode keeps the picture on the GPU (no CPU planes).
    const bool readable = f.format == PixFormat::NV12 || f.format == PixFormat::I420 || f.format == PixFormat::P016;
    const int why = f.gpu ? 1 : (!readable || f.plane[0].empty()) ? 2 : 0;
    unmeasurable_.store(why, std::memory_order_relaxed);
    if (why) {
        burst_.clear();
        burstBroken_ = false;
        burstStart_ = -1.0;
        return;
    }
    if (burst_.empty() && !burstBroken_) {
        std::lock_guard<std::mutex> lk(m_);
        if (pending_ || busy_ || now - lastSubmit_ < 0.25) return;  // ~4 measurements a second
    } else if (!burst_.empty() &&
               (f.serial != burstLastSerial_ + 1 || layout != burstLayout_ || swapEyes != burstSwap_)) {
        // A dropped frame or a change: no consecutive burst this time. The window keeps its
        // start, so after 0.5 s the latest frame is measured alone (spatial checks, no eye
        // sync), and the frames until then are not downsampled for nothing (render thread).
        // Restarting the window at every gap measured NOTHING while frames kept dropping:
        // UHD 59.94 on the station dropped every few frames and the rig check went blank.
        burst_.clear();
        burstBroken_ = true;
    }
    if (burstStart_ < 0.0) burstStart_ = now;
    if (burstBroken_ && now - burstStart_ < 0.5) return;  // wait out the window, then one frame
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
        burstBroken_ = false;
        burstStart_ = -1.0;
        return;
    }
    burst_.push_back(std::move(e));
    burstLastSerial_ = f.serial;
    burstLayout_ = layout;
    burstSwap_ = swapEyes;
    // Hand over once the burst is complete - or, when frames keep dropping, after 0.5 s
    // with the latest frame alone (spatial checks only, eye sync not measured).
    const bool complete = !burstBroken_ && (int)burst_.size() >= kSyncFrames;
    if (!complete && now - burstStart_ < 0.5) return;
    if (!complete) burst_.erase(burst_.begin(), burst_.end() - 1);
    burstStart_ = -1.0;  // the next window starts with the next collection
    burstBroken_ = false;
    std::lock_guard<std::mutex> lk(m_);
    work_ = std::move(burst_);
    burst_.clear();
    pendingEyeOffset_ = eyeOffsetFrames;
    pendingLayout_ = layout;
    pendingSwap_ = swapEyes;
    pendingTimecode_ = timecode;
    pendingStill_ = false;
    pending_ = true;
    lastSubmit_ = now;
    cv_.notify_one();
}

namespace {

// The still's kept copy: an integer box reduce by k to about kKeepMax px on its long side,
// so a re-measure (layout / eye swap) needs no decode and the 640 px analysis eyes still
// have 2x the pixels. k is picked to divide both half sizes where it can, so no reduced
// pixel straddles the two eyes of either packing.
constexpr int kKeepMax = 2560;

int StillReduceFactor(int w, int h) {
    const int k0 = std::max(1, (std::max(w, h) + kKeepMax - 1) / kKeepMax);
    for (int k = k0; k <= 2 * k0; ++k)
        if ((w / 2) % k == 0 && (h / 2) % k == 0) return k;
    return k0;
}

void ReduceRgba(const uint8_t* src, int w, int h, int k, std::vector<uint8_t>& dst, int& rw, int& rh) {
    rw = w / k;
    rh = h / k;
    dst.resize((size_t)rw * rh * 4);
    if (k == 1) {
        std::copy(src, src + dst.size(), dst.begin());
        return;
    }
    const uint32_t n = (uint32_t)(k * k);
    auto rows = [&](int y0, int y1) {
        std::vector<uint32_t> acc((size_t)rw * 4);
        for (int y = y0; y < y1; ++y) {
            std::fill(acc.begin(), acc.end(), 0u);
            for (int j = 0; j < k; ++j) {
                const uint8_t* s = src + (size_t)(y * k + j) * w * 4;
                for (int x = 0; x < rw; ++x)
                    for (int i = 0; i < k; ++i)
                        for (int c = 0; c < 4; ++c) acc[(size_t)x * 4 + c] += s[((size_t)x * k + i) * 4 + c];
            }
            uint8_t* d = dst.data() + (size_t)y * rw * 4;
            for (size_t i = 0; i < acc.size(); ++i) d[i] = (uint8_t)((acc[i] + n / 2) / n);
        }
    };
    const int chunks = std::clamp((int)std::thread::hardware_concurrency(), 1, 8);
    std::vector<std::future<void>> jobs;
    for (int c = 1; c < chunks; ++c)
        jobs.push_back(std::async(std::launch::async, rows, rh * c / chunks, rh * (c + 1) / chunks));
    rows(0, rh / chunks);
    for (auto& j : jobs) j.get();
}

}  // namespace

void RigMonitor::SubmitStill(const uint8_t* rgba, int w, int h, StereoLayout layout, bool swapEyes) {
    if (!rgba || w < 2 || h < 2) {
        ClearStill();
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    unmeasurable_.store(0, std::memory_order_relaxed);
    burst_.clear();
    burstBroken_ = false;
    burstStart_ = -1.0;
    stillK_ = StillReduceFactor(w, h);
    ReduceRgba(rgba, w, h, stillK_, still_, stillW_, stillH_);
    stillHeld_ = true;
    stillLayout_ = layout;
    stillSwap_ = swapEyes;
    {
        std::lock_guard<std::mutex> lk(m_);
        stillShown_ = true;
    }
    std::fprintf(stderr, "[mediaplayer][INFO ] Rig check (render thread): still %dx%d kept at %dx%d in %.2f ms\n", w, h,
                 stillW_, stillH_,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    MeasureStill();
}

void RigMonitor::UpdateStill(StereoLayout layout, bool swapEyes) {
    if (!stillHeld_ || (layout == stillLayout_ && swapEyes == stillSwap_)) return;
    stillLayout_ = layout;
    stillSwap_ = swapEyes;
    MeasureStill();
}

void RigMonitor::MeasureStill() {
    EyePair e;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = DownsampleRgba(still_.data(), stillW_, stillH_, stillLayout_, 640, e, stillSwap_);
    // the readings are in the source's pixels, not the kept copy's
    e.fullEyeW *= (float)stillK_;
    e.fullH *= (float)stillK_;
    if (ok)
        std::fprintf(stderr, "[mediaplayer][INFO ] Rig check (render thread): still downsample %.2f ms (%s%s)\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                     stillLayout_ == StereoLayout::TbFull || stillLayout_ == StereoLayout::TbHalf ? "top/bottom"
                                                                                                  : "side by side",
                     stillSwap_ ? ", eyes swapped" : "");
    std::lock_guard<std::mutex> lk(m_);
    work_.clear();
    if (ok) work_.push_back(std::move(e));  // nothing to measure (mono): the worker records "not measured"
    pendingEyeOffset_ = -1;
    pendingTimecode_.clear();
    pendingLayout_ = stillLayout_;
    pendingSwap_ = stillSwap_;
    pendingStill_ = true;
    pendingStillSerial_ = ++stillSerial_;
    stillMeasured_ = false;
    pending_ = true;
    cv_.notify_one();
}

void RigMonitor::ClearStill() {
    if (!stillHeld_) return;
    stillHeld_ = false;
    std::vector<uint8_t>().swap(still_);
    std::lock_guard<std::mutex> lk(m_);
    stillShown_ = stillMeasured_ = false;
    ++stillSerial_;  // a still measurement still running is dropped
}

bool RigMonitor::StillShown() const {
    std::lock_guard<std::mutex> lk(m_);
    return stillShown_;
}

const char* RigMonitor::Unmeasurable() const {
    switch (unmeasurable_.load(std::memory_order_relaxed)) {
        case 1: return "Rig check needs CPU frames: hardware decode is on (zero-copy)";
        case 2: return "Rig check cannot read this pixel format yet";
        default: return nullptr;
    }
}

namespace {
// The worker's cost, logged every 5 s (measurement aid, #268 acceptance): the measurement
// (with the eye-sync burst and any re-measure) and the dense heatmap pass.
struct WorkerLog {
    double sum[2] = {}, mx[2] = {}, next = 0.0;
    int n[2] = {};
    void Add(int k, double ms) {
        sum[k] += ms;
        mx[k] = std::max(mx[k], ms);
        ++n[k];
        const double now = LiveProducer::NowSeconds();
        if (now < next) return;
        if (next > 0.0) {
            if (n[1])
                std::fprintf(stderr,
                             "[mediaplayer][INFO ] Rig check (worker): measure %d x avg %.2f max %.2f ms, dense %d x avg "
                             "%.2f max %.2f ms\n",
                             n[0], n[0] ? sum[0] / n[0] : 0.0, mx[0], n[1], sum[1] / n[1], mx[1]);
            else
                std::fprintf(stderr, "[mediaplayer][INFO ] Rig check (worker): measure %d x avg %.2f max %.2f ms\n", n[0],
                             n[0] ? sum[0] / n[0] : 0.0, mx[0]);
        }
        *this = WorkerLog{};
        next = now + 5.0;
    }
};
WorkerLog g_workerLog;  // the worker thread only
}  // namespace

void RigMonitor::RunDense(std::unique_lock<std::mutex>& lk) {
    std::shared_ptr<const EyePair> p;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> pl(pairM_);
        p = lastPair_;
        seq = pairSeq_;
    }
    if (!p || seq == denseFor_) return;  // nothing new since the last map (a still, a pause)
    const uint64_t gen = generation_;
    lk.unlock();
    DenseMap d = MeasureDense(*p);
    lk.lock();
    denseFor_ = seq;
    g_workerLog.Add(1, d.ms);
    if (gen != generation_ || !denseOn_) return;
    d.seq = dense_.seq + 1;
    dense_ = std::move(d);
}

void RigMonitor::Worker() {
    std::unique_lock<std::mutex> lk(m_);
    auto nextDense = std::chrono::steady_clock::now();
    while (true) {
        // The dense pass (#268) runs only while nothing is queued: a hand-over always goes first.
        if (denseOn_)
            cv_.wait_until(lk, nextDense, [this] { return stop_ || pending_ || !denseOn_; });
        else
            cv_.wait(lk, [this] { return stop_ || pending_ || denseOn_; });
        if (stop_) return;
        if (!pending_) {
            if (denseOn_ && std::chrono::steady_clock::now() >= nextDense) {
                RunDense(lk);
                nextDense = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            }
            continue;
        }
        std::vector<EyePair> seq = std::move(work_);
        work_.clear();
        pending_ = false;  // a new hand-over may queue while this one is measured
        busy_ = true;
        const bool still = pendingStill_;
        const uint64_t stillSerial = pendingStillSerial_;
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
            const float shiftY = e.PxPerRow() > 0 ? r.verticalPx / e.PxPerRow() : 0.f;
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
        busy_ = false;
        if (gen != generation_) continue;  // Reset() ran meanwhile: this was the old input
        if (still && stillSerial != stillSerial_) continue;  // a newer still view (or frames) replaced it
        measureMs_ = ms;
        g_workerLog.Add(0, ms);
        last_ = r;
        if (!seq.empty()) {
            // the cursor probe and the dense pass read the measured pair (#268)
            auto pair = std::make_shared<const EyePair>(std::move(seq.back()));
            std::lock_guard<std::mutex> pl(pairM_);
            lastPair_ = std::move(pair);
            ++pairSeq_;
        }
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
        for (int i = 0; i < 2; ++i) {
            s.edgeMeasured[i] = r.edge[i].measured;
            s.edgePct[i] = r.edge[i].parallaxPct;
        }
        if (still) {
            // A still has no duration: its sample joins the history (strip, report) but
            // opens no alert run, and the runs the frames before it left open end at
            // their last good sample.
            stillMeasured_ = true;
            history_.push_back(s);
            if (lastGoodT_ >= 0)
                for (int i = 0; i < kRuns; ++i) CloseRun(i, lastGoodT_, lastGoodTc_);
        } else {
            AddSampleLocked(s, tc);
        }
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
// The alert texts, per run: near, far, vertical, colour, edges, target divergence.
const char* const kClosedFmt[7] = {
    "Foreground at %+.1f%% for %.0f s (near limit %+.1f%%)",
    "Background at %+.1f%% for %.0f s (far limit %+.1f%%)",
    "Vertical error %.1f px for %.0f s (limit %.1f px)",
    "Colour mismatch dE %.1f for %.0f s (limit %.1f)",
    "Left edge violation at %+.1f%% for %.0f s (edge limit %+.1f%%)",
    "Right edge violation at %+.1f%% for %.0f s (edge limit %+.1f%%)",
    "Background diverges on the target screen at %+.1f%% for %.0f s (limit %+.1f%%)",
};
const char* const kOpenFmt[7] = {
    "Foreground at %+.1f%% for %.0f s (now)",
    "Background at %+.1f%% for %.0f s (now)",
    "Vertical error %.1f px for %.0f s (now)",
    "Colour mismatch dE %.1f for %.0f s (now)",
    "Left edge violation at %+.1f%% for %.0f s (now)",
    "Right edge violation at %+.1f%% for %.0f s (now)",
    "Background diverges on the target screen at %+.1f%% for %.0f s (now)",
};
// #135: a depth alert's worst value in the target screen's terms, appended to its text:
// " [-201 mm, -58' on Cinema]", or the divergence angle for the divergence run. Empty
// without a target, and for the vertical / colour runs. `worst` is the source's parallax,
// except for the divergence run, which already includes `offsetPct` (convergence).
std::string TargetNote(const Budget& b, int run, float worst, float offsetPct) {
    if (b.targetWidthM <= 0.0f || b.targetDistanceM <= 0.0f) return {};
    if (!(run == 0 || run == 1 || run == 4 || run == 5 || run == 6)) return {};
    const mp::pro::target::Depth d = mp::pro::target::AtParallax({b.targetName, b.targetWidthM, b.targetDistanceM},
                                                                 run == 6 ? worst : worst + offsetPct);
    char s[96];
    if (run == 6) std::snprintf(s, sizeof(s), " [%.2f deg on %s]", d.divergenceDeg, b.targetName.c_str());
    else std::snprintf(s, sizeof(s), " [%+.0f mm, %+.0f' on %s]", d.mm, d.arcmin, b.targetName.c_str());
    return s;
}

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
        const float limits[kRuns] = {budget_.nearPct, budget_.farPct, budget_.maxVerticalPx,
                                     budget_.maxDeltaE, budget_.edgePct, budget_.edgePct,
                                     budget_.divergeFarPct};
        Alert a;
        a.start = r.since;
        a.end = end;
        char b[160];
        std::snprintf(b, sizeof(b), kClosedFmt[i], r.worst, std::max(1.0, std::round(dur)), limits[i]);
        a.what = std::string(b) + TargetNote(budget_, i, r.worst, targetParallaxOffsetPct_);
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
            for (int i = 0; i < kRuns; ++i) CloseRun(i, lastGoodT_, lastGoodTc_);
        return;
    }
    notOkSince_ = -1;
    lastGoodT_ = s.t;
    lastGoodTc_ = tc;
    const float shownFarPct = s.farPct + targetParallaxOffsetPct_;
    const bool over[kRuns] = {s.nearPct < budget_.nearPct,
                              s.farPct > budget_.farPct,
                              std::fabs(s.verticalPx) > budget_.maxVerticalPx,
                              s.deltaE > budget_.maxDeltaE,
                              s.edgeMeasured[0] && s.edgePct[0] < budget_.edgePct,
                              s.edgeMeasured[1] && s.edgePct[1] < budget_.edgePct,
                              budget_.divergeFarPct > 0.0f && shownFarPct > budget_.divergeFarPct};
    const float value[kRuns] = {s.nearPct, s.farPct, std::fabs(s.verticalPx), s.deltaE, s.edgePct[0], s.edgePct[1],
                                shownFarPct};
    for (int i = 0; i < kRuns; ++i) {
        Run& r = runs_[i];
        if (over[i]) {
            if (r.since < 0) {
                r.since = s.t;
                r.worst = value[i];
                r.tc = tc;
                r.clock = WallClock(s.t);
            } else {
                const bool lowerIsWorse = i == 0 || i == 4 || i == 5;  // near and the edges: further in front is worse
                r.worst = lowerIsWorse ? std::min(r.worst, value[i]) : std::max(r.worst, value[i]);
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

void RigMonitor::SetTargetParallaxOffset(float pct) {
    std::lock_guard<std::mutex> lk(m_);
    targetParallaxOffsetPct_ = pct;
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
    {
        const uint64_t s = dense_.seq;  // kept rising: the UI re-uploads on a new seq
        dense_ = DenseMap{};
        dense_.seq = s;
        std::lock_guard<std::mutex> pl(pairM_);
        lastPair_.reset();
    }
    for (Run& r : runs_) r.since = -1;
    lastGoodT_ = notOkSince_ = -1.0;
    lastGoodTc_.clear();
    unmeasurable_.store(0, std::memory_order_relaxed);
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
    for (int i = 0; i < kRuns; ++i) {
        const Run& r = runs_[i];
        if (r.since < 0 || now - r.since < 0.5) continue;
        Alert x;
        x.start = r.since;
        x.end = -1.0;
        char b[160];
        std::snprintf(b, sizeof(b), kOpenFmt[i], r.worst, now - r.since);
        x.what = std::string(b) + TargetNote(budget_, i, r.worst, targetParallaxOffsetPct_);
        x.timecode = r.tc;
        x.wallClock = r.clock;
        a.push_back(x);
    }
    return a;
}

bool RigMonitor::Active(double now) const {
    std::lock_guard<std::mutex> lk(m_);
    if (stillShown_) return stillMeasured_;  // a still is measured once and holds
    return !history_.empty() && now - history_.back().t < 2.0;
}

double RigMonitor::LastMeasureMs() const {
    std::lock_guard<std::mutex> lk(m_);
    return measureMs_;
}

Probe RigMonitor::ProbeParallax(float u, float v, float& pct, float& conf) const {
    std::shared_ptr<const EyePair> p;
    {
        std::lock_guard<std::mutex> pl(pairM_);
        p = lastPair_;
    }
    pct = conf = 0.f;
    return p ? ProbeAt(*p, u, v, pct, conf) : Probe::None;
}

void RigMonitor::RequestProbe(float u, float v) {
    {
        std::lock_guard<std::mutex> pl(pairM_);
        probeU_ = u;
        probeV_ = v;
        probeReq_ = true;
        if (!probeThread_.joinable()) probeThread_ = std::thread([this] { ProbeWorker(); });
    }
    probeCv_.notify_one();
}

bool RigMonitor::ProbeResult(Probe& state, float& pct, float& conf) const {
    std::lock_guard<std::mutex> pl(pairM_);
    state = probeState_;
    pct = probePct_;
    conf = probeConf_;
    return probeDone_;
}

// A larger block on a smooth area costs up to ~1 ms: kept off the render thread, and off
// the rig worker, which can be busy with a measurement for longer than a cursor can wait.
void RigMonitor::ProbeWorker() {
    std::unique_lock<std::mutex> pl(pairM_);
    while (true) {
        probeCv_.wait(pl, [this] { return probeStop_ || probeReq_; });
        if (probeStop_) return;
        probeReq_ = false;
        const float u = probeU_, v = probeV_;
        const std::shared_ptr<const EyePair> p = lastPair_;
        pl.unlock();
        float pct = 0.f, conf = 0.f;
        const Probe s = p ? ProbeAt(*p, u, v, pct, conf) : Probe::None;
        pl.lock();
        probeState_ = s;
        probePct_ = pct;
        probeConf_ = conf;
        probeDone_ = true;
    }
}

void RigMonitor::SetDenseEnabled(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (denseOn_ == on) return;
        denseOn_ = on;
        denseFor_ = 0;  // on again: map the current pair at once
        if (!on) {
            const uint64_t s = dense_.seq;
            dense_ = DenseMap{};
            dense_.seq = s;
        }
    }
    cv_.notify_all();
}

DenseMap RigMonitor::Dense() const {
    std::lock_guard<std::mutex> lk(m_);
    return dense_;
}

}  // namespace mp::rig
