// SPDX-License-Identifier: Apache-2.0
#include "pro/RigCheck.h"

#include "media/LiveProducer.h"
#include "pro/TargetScreen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <future>
#include <iterator>
#include <limits>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
#include <emmintrin.h>
#define RIG_NCC_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define RIG_NCC_NEON 1
#endif

namespace mp::rig {

namespace {

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
// The matcher's inner loop, so 4 lanes wide where the baseline ISA has them (SSE2 on
// x86-64, NEON on arm64): ~3x the scalar loop, which no compiler vectorises without
// fast-math (#281). The sums come out in a different order, a few ulp from the scalar.
float Ncc(const std::vector<float>& R, int eyeW, const float* tpl, float tplNorm, int bs, int x, int y) {
    float sum = 0, sum2 = 0, cross = 0;
    const int b4 = bs & ~3;
#if defined(RIG_NCC_SSE2) || defined(RIG_NCC_NEON)
#if defined(RIG_NCC_SSE2)
    __m128 vs = _mm_setzero_ps(), vs2 = _mm_setzero_ps(), vc = _mm_setzero_ps();
#else
    float32x4_t vs = vdupq_n_f32(0.f), vs2 = vdupq_n_f32(0.f), vc = vdupq_n_f32(0.f);
#endif
    for (int j = 0; j < bs; ++j) {
        const float* r = R.data() + (size_t)(y + j) * eyeW + x;
        const float* t = tpl + j * bs;
        for (int i = 0; i < b4; i += 4) {
#if defined(RIG_NCC_SSE2)
            const __m128 v = _mm_loadu_ps(r + i);
            vs = _mm_add_ps(vs, v);
            vs2 = _mm_add_ps(vs2, _mm_mul_ps(v, v));
            vc = _mm_add_ps(vc, _mm_mul_ps(v, _mm_loadu_ps(t + i)));
#else
            const float32x4_t v = vld1q_f32(r + i);
            vs = vaddq_f32(vs, v);
            vs2 = vmlaq_f32(vs2, v, v);
            vc = vmlaq_f32(vc, v, vld1q_f32(t + i));
#endif
        }
        for (int i = b4; i < bs; ++i) {
            const float v = r[i];
            sum += v;
            sum2 += v * v;
            cross += v * t[i];
        }
    }
    float l[4];
#if defined(RIG_NCC_SSE2)
    _mm_storeu_ps(l, vs);
    sum += (l[0] + l[1]) + (l[2] + l[3]);
    _mm_storeu_ps(l, vs2);
    sum2 += (l[0] + l[1]) + (l[2] + l[3]);
    _mm_storeu_ps(l, vc);
    cross += (l[0] + l[1]) + (l[2] + l[3]);
#else
    vst1q_f32(l, vs);
    sum += (l[0] + l[1]) + (l[2] + l[3]);
    vst1q_f32(l, vs2);
    sum2 += (l[0] + l[1]) + (l[2] + l[3]);
    vst1q_f32(l, vc);
    cross += (l[0] + l[1]) + (l[2] + l[3]);
#endif
#else
    (void)b4;
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
#endif
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

// #270: one eye's colour samples, in 12-bit code values (0..4095): the R'G'B' + luma
// histograms and sums over the overlap, and the colour grid over the whole eye. All
// integers, so any split of the rows over threads sums to the same.
struct ColourAcc {
    uint32_t hist[4][kHistBins] = {};
    uint64_t sum[4] = {};
    int64_t n = 0;
    std::vector<uint32_t> grid;  // kColourGridW * kColourGridH * 4: R, G, B sums + count (< 2^32 per cell)
};

// Colour samples are taken every kColourStep columns (one per 2x2 block, every other block) of
// every kColourRowStep-th row: every 2nd column of every 7th row cost 60 % on top of the
// downsample at 7680x2160, for no gain in the figures.
constexpr int kColourStep = 4, kColourRowStep = 7;

inline int To12(float v) { return std::clamp((int)(v * 4095.f + 0.5f), 0, 4095); }

// The downsample shared by every pixel format: a box filter of each eye to the analysis
// size and each eye's colour samples, on several threads. The format lives in the callbacks:
//   addRow(eye, row, colSum)       add source row `row` (frame coordinates) of that eye's
//                                  columns into colSum (g.w integer sums of luma)
//   colourRow(eye, row, emit)      for every kColourStep-th column x of that eye's eye-local row `row`,
//                                  emit(x, r, g, b, luma) in 12-bit R'G'B' code values
// unit scales the luma sums to 0..255. Each analysis pixel and each colour figure is
// computed exactly as on one thread (each output row is independent; the colour sums are
// integers, whose total does not depend on their order).
template <class AddRow, class ColourRow>
void DownsampleEyes(const EyeGeom& g, int analysisW, float unit, AddRow addRow, ColourRow colourRow, EyePair& out) {
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
    // Colour: whole rows, every kColourRowStep-th, every kColourStep-th column. A sparse grid aliases with periodic
    // content (a 16 px grid on a drifting 160 px grating read one eye 15 % brighter for a
    // burst now and then, and the colour alert latched that peak - tmp-mp-pro 2026-10-18).
    // #270: the histograms, means and dE are over the OVERLAP only: within the parallax
    // search range of an eye's edge (8 %) is scene the other eye may not see, so a bright
    // object there moved dE with no camera mismatch. Both edge bands of both eyes are left
    // out: dropping only each eye's outer band compared two different 8 % strips of every
    // screen-plane object, a 0.1-0.4 % gain on matched eyes. Rows within 2 % of the top /
    // bottom and matte rows (flat and dark: letterbox) are left out too.
    const int rowFirst = g.h * 2 / 100, rowEnd = g.h * 98 / 100;
    const int rowCount = rowFirst < rowEnd ? (rowEnd - rowFirst + kColourRowStep - 1) / kColourRowStep : 0;
    const int ov = (int)std::lround(kOverlapEdge * g.w);
    std::vector<uint16_t> cellOf((size_t)g.w);  // column -> colour grid column (no division per sample)
    for (int x = 0; x < g.w; ++x) cellOf[x] = (uint16_t)(x * kColourGridW / g.w);
    auto colourRows = [&](int eye, int kBegin, int kEnd) {
        ColourAcc a;
        a.grid.assign((size_t)kColourGridW * kColourGridH * 4, 0);
        const int xLo = ov, xHi = g.w - ov;
        // Straight into the totals; a matte row (rare) is taken out again afterwards. Row-
        // local histograms merged per row cost more than the samples themselves.
        auto add = [&](int y, uint32_t sign, uint64_t* rs, uint64_t* l2, int64_t* rn) {
            uint32_t* gr = a.grid.data() + (size_t)std::min(kColourGridH - 1, y * kColourGridH / g.h) * kColourGridW * 4;
            colourRow(eye, y, [&](int x, int r, int gg, int b, int l) {
                uint32_t* c = gr + (size_t)cellOf[x] * 4;
                c[0] += sign * (uint32_t)r;  // sign: 1, or ~0u (= -1 mod 2^32) to take a row out
                c[1] += sign * (uint32_t)gg;
                c[2] += sign * (uint32_t)b;
                c[3] += sign;
                if (x < xLo || x >= xHi) return;
                a.hist[0][r >> kHistShift] += sign;
                a.hist[1][gg >> kHistShift] += sign;
                a.hist[2][b >> kHistShift] += sign;
                a.hist[3][l >> kHistShift] += sign;
                if (!rs) return;
                rs[0] += (uint64_t)r;
                rs[1] += (uint64_t)gg;
                rs[2] += (uint64_t)b;
                rs[3] += (uint64_t)l;
                *l2 += (uint64_t)l * (uint64_t)l;
                ++*rn;
            });
        };
        for (int k = kBegin; k < kEnd; ++k) {
            const int y = rowFirst + kColourRowStep * k;
            uint64_t rs[4] = {}, l2 = 0;
            int64_t rn = 0;
            add(y, 1u, rs, &l2, &rn);
            if (rn > 0) {  // a matte row would pile both eyes' histograms into black
                const double m = (double)rs[3] / (double)rn, var = (double)l2 / (double)rn - m * m;
                if (m < 0.08 * 4095.0 && var < 25.0 * 25.0) {
                    add(y, ~0u, nullptr, nullptr, nullptr);
                    continue;
                }
            }
            for (int c = 0; c < 4; ++c) a.sum[c] += rs[c];
            a.n += rn;
        }
        return a;
    };
    // Threads per eye: up to 4. MEDIAPLAYER_RIG_THREADS=<n> pins it (tests compare 1 vs n).
    int chunks = std::clamp((int)std::thread::hardware_concurrency() / 2, 1, 4);
    if (const char* t = std::getenv("MEDIAPLAYER_RIG_THREADS")) chunks = std::clamp(std::atoi(t), 1, 16);
    std::vector<std::future<void>> jobs;
    std::vector<std::future<ColourAcc>> colour[2];
    for (int eye = 0; eye < 2; ++eye)
        for (int c = 0; c < chunks; ++c) {
            colour[eye].push_back(std::async(std::launch::async, colourRows, eye, rowCount * c / chunks,
                                             rowCount * (c + 1) / chunks));
            const int y0 = ah * c / chunks, y1 = ah * (c + 1) / chunks;
            if (c == chunks - 1 && eye == 1) continue;  // the last chunk runs here
            jobs.push_back(std::async(std::launch::async, lumaRows, eye, y0, y1));
        }
    lumaRows(1, ah * (chunks - 1) / chunks, ah);
    for (auto& j : jobs) j.get();
    for (int eye = 0; eye < 2; ++eye) {
        ColourAcc t;
        t.grid.assign((size_t)kColourGridW * kColourGridH * 4, 0);
        for (auto& f : colour[eye]) {
            const ColourAcc a = f.get();
            for (int c = 0; c < 4; ++c) {
                for (int b = 0; b < kHistBins; ++b) t.hist[c][b] += a.hist[c][b];
                t.sum[c] += a.sum[c];
            }
            t.n += a.n;
            for (size_t i = 0; i < t.grid.size(); ++i) t.grid[i] += a.grid[i];
        }
        std::memcpy(out.hist[eye], t.hist, sizeof(t.hist));
        out.colourSamples[eye] = t.n;
        if (t.n > 0) {
            for (int c = 0; c < 3; ++c) out.meanRgb[eye][c] = (float)((double)t.sum[c] / (double)t.n / 4095.0);
            out.meanLuma[eye] = (float)((double)t.sum[3] / (double)t.n / 4095.0);
        }
        std::vector<float>& gd = out.grid[eye];
        gd.assign((size_t)kColourGridW * kColourGridH * 3, std::nanf(""));
        for (int i = 0; i < kColourGridW * kColourGridH; ++i) {
            const uint32_t* s = t.grid.data() + (size_t)i * 4;
            if (!s[3]) continue;
            for (int c = 0; c < 3; ++c) gd[(size_t)i * 3 + c] = (float)((double)s[c] / (double)s[3] / 4095.0);
        }
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
    // #270: limited/full-range BT.709 Y'CbCr -> R'G'B' code values per sample (the mean used
    // to be converted once); the source's code values, before the grade.
    const float s = p16 ? 1.f / 256.f : 1.f;
    const float yOff = f.fullRange ? 0.f : 16.f, yK = (f.fullRange ? 1.f / 255.f : 1.f / 219.f) * s;
    const float cK = (f.fullRange ? 1.f / 255.f : 1.f / 224.f) * s, yO = yOff / s, cO = 128.f / s;
    auto toRgb = [&](int x, float Y, unsigned U, unsigned V, auto& emit) {
        const float yb = (Y - yO) * yK, ub = ((float)U - cO) * cK, vb = ((float)V - cO) * cK;
        emit(x, To12(yb + 1.5748f * vb), To12(yb - 0.1873f * ub - 0.4681f * vb), To12(yb + 1.8556f * ub), To12(yb));
    };
    // One sample per chroma sample: the 2x2 luma block it covers, averaged, with its chroma.
    // That is exactly the block's mean R'G'B' (the conversion is linear), where a single
    // luma pixel with the shared chroma is not: a gain on one channel then moved the
    // others' histograms (4:2:0 crosstalk, up to 2.5 % on blue for a 3 % green gain).
    const int H = f.height;
    auto colourRow = [&](int eye, int yEye, auto&& emit) {
        const int y = std::min((g.y0[eye] + yEye) & ~1, H - 2), x0 = g.x0[eye];
        if (p16) {
            const uint16_t* Y0 = reinterpret_cast<const uint16_t*>(f.plane[0].data()) + (size_t)y * W;
            const uint16_t* Y1 = Y0 + W;
            const uint16_t* UV = reinterpret_cast<const uint16_t*>(f.plane[1].data()) + (size_t)(y / 2) * W;
            for (int x = 0; x + 1 < g.w; x += kColourStep) {
                const int gx = (x0 + x) & ~1;
                toRgb(x, (float)(Y0[gx] + Y0[gx + 1] + Y1[gx] + Y1[gx + 1]) * 0.25f, UV[gx], UV[gx + 1], emit);
            }
        } else if (f.format == PixFormat::NV12) {
            const uint8_t* Y0 = f.plane[0].data() + (size_t)y * W;
            const uint8_t* Y1 = Y0 + W;
            const uint8_t* UV = f.plane[1].data() + (size_t)(y / 2) * W;
            for (int x = 0; x + 1 < g.w; x += kColourStep) {
                const int gx = (x0 + x) & ~1;
                toRgb(x, (Y0[gx] + Y0[gx + 1] + Y1[gx] + Y1[gx + 1]) * 0.25f, UV[gx], UV[gx + 1], emit);
            }
        } else {
            const uint8_t* Y0 = f.plane[0].data() + (size_t)y * W;
            const uint8_t* Y1 = Y0 + W;
            const uint8_t* U = f.plane[1].data() + (size_t)(y / 2) * cw;
            const uint8_t* V = f.plane[2].data() + (size_t)(y / 2) * cw;
            for (int x = 0; x + 1 < g.w; x += kColourStep) {
                const int gx = (x0 + x) & ~1;
                toRgb(x, (Y0[gx] + Y0[gx + 1] + Y1[gx] + Y1[gx + 1]) * 0.25f, U[gx / 2], V[gx / 2], emit);
            }
        }
    };
    DownsampleEyes(g, analysisW, p16 ? 1.0f / 256.0f : 1.0f, addRow, colourRow, out);
    out.pq = f.transfer == ColorTransfer::PQ;
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
    auto colourRow = [&](int eye, int yEye, auto&& emit) {
        const uint8_t* src = rgba + ((size_t)(g.y0[eye] + yEye) * w + g.x0[eye]) * 4;
        for (int x = 0; x < g.w; x += kColourStep, src += 4 * kColourStep) {
            const unsigned r = src[0], gg = src[1], b = src[2];
            emit(x, (int)((r * 4095u + 127u) / 255u), (int)((gg * 4095u + 127u) / 255u), (int)((b * 4095u + 127u) / 255u),
                 (int)(((54u * r + 183u * gg + 19u * b) * 4095u + 32640u) / 65280u));
        }
    };
    DownsampleEyes(g, analysisW, 1.0f / 256.0f, addRow, colourRow, out);
    return true;
}

// ---------------------------------------------------------------------------------------
// #270: colour between the eyes

namespace {

constexpr double kBinW = 1 << kHistShift;  // 12-bit codes per histogram bin

// The value (0..1) below which a fraction q of a histogram's samples lie, linear inside the
// bin (bin b holds the 12-bit codes kBinW * b .. kBinW * (b + 1) - 1).
double Quantile(const uint32_t* h, double total, double q) {
    const double target = q * total;
    double acc = 0;
    for (int b = 0; b < kHistBins; ++b) {
        if (h[b] && acc + h[b] >= target) return kBinW * (b + (target - acc) / h[b]) / 4095.0;
        acc += h[b];
    }
    return 1.0;
}

}  // namespace

int ColourMatch::WorstChannel() const {
    int w = 0;
    for (int c = 1; c < 3; ++c)
        if (std::fabs(gainPct[c]) > std::fabs(gainPct[w])) w = c;
    return w;
}

ColourMatch MeasureColour(const EyePair& e) {
    ColourMatch m;
    m.pq = e.pq;
    if (e.colourSamples[0] < 1000 || e.colourSamples[1] < 1000) return m;
    m.measured = true;
    for (int c = 0; c < 3; ++c) {
        const uint32_t* hl = e.hist[0][c];
        const uint32_t* hr = e.hist[1][c];
        const double nl = (double)e.colourSamples[0], nr = (double)e.colourSamples[1];
        // Histogram matching reduced to two numbers: the eyes' p5, p10 .. p95 paired up and
        // right = gain * left + lift fitted through them. Position-free, so parallax does
        // not matter, and percentiles shrug off small occlusions. Pairs at the clip points
        // carry no slope and are left out.
        double sx = 0, sy = 0, sxx = 0, sxy = 0, lo = 1, hi = 0;
        int n = 0;
        for (int k = 1; k <= 19; ++k) {
            const double xl = Quantile(hl, nl, 0.05 * k), xr = Quantile(hr, nr, 0.05 * k);
            if (xl <= 0.004 || xl >= 0.996 || xr <= 0.004 || xr >= 0.996) continue;
            sx += xl;
            sy += xr;
            sxx += xl * xl;
            sxy += xl * xr;
            lo = std::min(lo, xl);
            hi = std::max(hi, xl);
            ++n;
        }
        const double var = n > 0 ? sxx - sx * sx / n : 0.0;
        if (n < 6 || hi - lo < 0.05 || var <= 1e-9) {  // a flat channel: nothing to fit a slope to
            m.measured = false;
            continue;
        }
        const double gain = (sxy - sx * sy / n) / var, lift = (sy - gain * sx) / n;
        m.gainPct[c] = (float)((gain - 1.0) * 100.0);
        m.liftPct[c] = (float)(lift * 100.0);
        // Shape: the left histogram carried through the fit, against the right one
        // (intersection, 1 = the same). A gamma or curve mismatch, which a gain and lift
        // cannot express, leaves it lower.
        double pred[kHistBins] = {};
        for (int b = 0; b < kHistBins; ++b) {
            if (!hl[b]) continue;
            const double a0 = gain * kBinW * b / 4095.0 + lift, a1 = gain * kBinW * (b + 1) / 4095.0 + lift;
            const double x0 = std::min(a0, a1) * 4095.0 / kBinW, x1 = std::max(a0, a1) * 4095.0 / kBinW;
            const double mass = hl[b] / nl;
            if (x1 - x0 < 1e-9) {
                pred[std::clamp((int)x0, 0, kHistBins - 1)] += mass;
                continue;
            }
            for (int t = std::max(0, (int)std::floor(x0)); t <= std::min(kHistBins - 1, (int)std::floor(x1)); ++t)
                pred[t] += mass * (std::min(x1, t + 1.0) - std::max(x0, (double)t)) / (x1 - x0);
            if (x0 < 0) pred[0] += mass * (std::min(x1, 0.0) - x0) / (x1 - x0);  // past the clip points
            if (x1 > kHistBins) pred[kHistBins - 1] += mass * (x1 - std::max(x0, (double)kHistBins)) / (x1 - x0);
        }
        // compared in 64 bins: at the histogram's 256 a camera's noise alone, which widens
        // each peak by a bin or two, read as a shape mismatch
        double inter = 0;
        constexpr int kGroup = kHistBins / 64;
        for (int b = 0; b < kHistBins; b += kGroup) {
            double p = 0, q = 0;
            for (int i = 0; i < kGroup; ++i) {
                p += pred[b + i];
                q += hr[b + i] / nr;
            }
            inter += std::min(p, q);
        }
        m.shape[c] = (float)inter;
    }
    if (e.meanLuma[0] > 1e-3f && e.meanLuma[1] > 1e-3f) {
        const double ratio = (double)e.meanLuma[1] / e.meanLuma[0];
        m.lumaPct = (float)((ratio - 1.0) * 100.0);
        m.lumaStops = (float)std::log2(ratio);
    }
    return m;
}

ColourDiff MeasureColourDiff(const EyePair& e, float shiftPct) {
    ColourDiff d;
    if ((int)e.grid[0].size() != kColourGridW * kColourGridH * 3 || e.grid[1].size() != e.grid[0].size()) return d;
    d.u0 = kOverlapEdge;
    d.u1 = 1.f - kOverlapEdge;
    d.v0 = 0.02f;
    d.v1 = 0.98f;
    const size_t cells = (size_t)d.cols * d.rows;
    d.y.assign(cells, std::nanf(""));
    d.cb.assign(cells, std::nanf(""));
    d.cr.assign(cells, std::nanf(""));
    // The mean colour of [ua, ub) x [va, vb) (eye fractions) from one eye's grid, weighted by
    // overlap; false when under half of it has samples (matte rows, past the eye's edge).
    auto box = [](const std::vector<float>& gd, float ua, float ub, float va, float vb, float rgb[3]) {
        const float fx0 = ua * kColourGridW, fx1 = ub * kColourGridW, fy0 = va * kColourGridH, fy1 = vb * kColourGridH;
        double acc[3] = {}, wsum = 0;
        for (int j = std::max(0, (int)fy0); j < std::min(kColourGridH, (int)std::ceil(fy1)); ++j) {
            const float wy = std::min(fy1, j + 1.f) - std::max(fy0, (float)j);
            for (int i = std::max(0, (int)fx0); i < std::min(kColourGridW, (int)std::ceil(fx1)); ++i) {
                const float* c = &gd[((size_t)j * kColourGridW + i) * 3];
                if (std::isnan(c[0])) continue;
                const float w = wy * (std::min(fx1, i + 1.f) - std::max(fx0, (float)i));
                if (w <= 0.f) continue;
                for (int k = 0; k < 3; ++k) acc[k] += w * c[k];
                wsum += w;
            }
        }
        if (wsum < 0.5 * (fx1 - fx0) * (fy1 - fy0)) return false;
        for (int k = 0; k < 3; ++k) rgb[k] = (float)(acc[k] / wsum);
        return true;
    };
    // The heavy low-pass makes the parallax irrelevant at this scale once the right eye is
    // sampled at the median parallax (parallax = x_right - x_left).
    const float shift = shiftPct / 100.f;
    const float du = (d.u1 - d.u0) / d.cols, dv = (d.v1 - d.v0) / d.rows;
    for (int j = 0; j < d.rows; ++j)
        for (int i = 0; i < d.cols; ++i) {
            const float ua = d.u0 + i * du, va = d.v0 + j * dv;
            float l[3], r[3];
            if (!box(e.grid[0], ua, ua + du, va, va + dv, l) || !box(e.grid[1], ua + shift, ua + du + shift, va, va + dv, r))
                continue;
            auto Y = [](const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
            const float yl = Y(l), yr = Y(r);
            const size_t k = (size_t)j * d.cols + i;
            d.y[k] = yr - yl;
            d.cb[k] = ((r[2] - yr) - (l[2] - yl)) / 1.8556f;
            d.cr[k] = ((r[0] - yr) - (l[0] - yl)) / 1.5748f;
        }
    return d;
}

std::string ColourFixCc(const ColourMatch& c) {
    // right = g * left + o, so left = right / g - o / g: slope 1 / g, offset -o / g
    float slope[3], offset[3];
    for (int k = 0; k < 3; ++k) {
        const double g = 1.0 + c.gainPct[k] / 100.0, o = c.liftPct[k] / 100.0;
        slope[k] = g > 1e-3 ? (float)(1.0 / g) : 1.f;
        offset[k] = g > 1e-3 ? (float)(-o / g) : 0.f;
    }
    char b[640];
    std::snprintf(b, sizeof(b),
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<ColorCorrection id=\"right_eye_to_left\">\n"
                  "  <Description>DisplayXR Media Player Pro rig check: right eye matched to the left "
                  "(R %+.2f%% G %+.2f%% B %+.2f%%%s)</Description>\n"
                  "  <SOPNode>\n"
                  "    <Slope>%.6f %.6f %.6f</Slope>\n"
                  "    <Offset>%.6f %.6f %.6f</Offset>\n"
                  "    <Power>1.000000 1.000000 1.000000</Power>\n"
                  "  </SOPNode>\n"
                  "  <SatNode>\n"
                  "    <Saturation>1.000000</Saturation>\n"
                  "  </SatNode>\n"
                  "</ColorCorrection>\n",
                  c.gainPct[0], c.gainPct[1], c.gainPct[2], c.pq ? ", PQ code values" : "", slope[0], slope[1], slope[2],
                  offset[0], offset[1], offset[2]);
    return b;
}

namespace {

// One block: the template at (bx, by) of `a`, searched for in `b` over dx in [-R, R] and
// dy in [dyC - D, dyC + D], clipped to the frame (NCC, coarse then fine, sub-pixel peak,
// uniqueness against periodic texture). On success dx / dy are where it sits in `b` minus
// where it sits in `a`, analysis px. tpl is scratch (bs * bs).
bool MatchAt(const std::vector<float>& a, const std::vector<float>& b, int W, int H, const Options& o, int bx, int by,
             int R, int D, int dyC, std::vector<float>& tpl, float& outDx, float& outDy,
             float* outScore = nullptr, int dxC = 0) {
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
    const int dxLo = std::max(dxC - R, -bx), dxHi = std::min(dxC + R, W - bs - bx);
    const int dyLo = std::max(dyC - D, -by), dyHi = std::min(dyC + D, H - bs - by);
    if (dyHi - dyLo < 4) return false;
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

// #269: the centre of the vertical search for a block centred at (cx, cy), from the seed
// (Options::seedDy*): a previous fit's offset, roll and zoom. Capped at a quarter height.
int SeedDy(const Options& o, int W, int H, float cx, float cy) {
    const float d = o.seedDy0 + o.seedDyX * (cx - 0.5f * W) + o.seedDyY * (cy - 0.5f * H);
    return (int)std::lround(std::clamp(d, -0.25f * H, 0.25f * H));
}

// Block-match `right` against templates cut from `left` (MatchAt). One entry per accepted
// block: its parallax dx, vertical offset dy (analysis px, right minus left) and template
// x; the normalised centre goes to r when given. With `mask`, only blocks whose centre is
// marked (want = true) or not marked (want = false) are matched.
void MatchBlocks(const std::vector<float>& left, const std::vector<float>& right, int W, int H, const Options& o,
                 const std::vector<uint8_t>* mask, bool want, std::vector<float>& dxs, std::vector<float>& dys,
                 std::vector<float>& xs, Result* r, std::vector<int>* ids = nullptr, int band = 0, int bands = 1) {
    const int bs = o.block;
    const int R = std::max(2, (int)std::lround(o.maxParallaxPct / 100.f * W));
    const int D = o.maxDy;
    std::vector<float> tpl((size_t)bs * bs);
    // band / bands: only that contiguous share of the grid's rows (MatchBlocksParallel)
    const int rows = H - 2 * D - 4 - bs >= 0 ? (H - 2 * D - 4 - bs) / o.step + 1 : 0;
    const int rowLo = rows * band / bands, rowHi = rows * (band + 1) / bands;
    for (int by = D + 2 + rowLo * o.step; by + bs + D + 2 <= H && by < D + 2 + rowHi * o.step; by += o.step) {
        for (int bx = 2; bx + bs + 2 <= W; bx += o.step) {
            if (mask && ((*mask)[(size_t)(by + bs / 2) * W + bx + bs / 2] != 0) != want) continue;
            float dx, dy;
            const int dyC = SeedDy(o, W, H, bx + bs * 0.5f, by + bs * 0.5f);
            if (!MatchAt(left, right, W, H, o, bx, by, R, D, dyC, tpl, dx, dy)) continue;
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

// MatchBlocks over `bands` row bands on their own threads (#281), joined in row order: the
// same blocks in the same order as one MatchBlocks call. Appends to the outputs.
void MatchBlocksParallel(const std::vector<float>& left, const std::vector<float>& right, int W, int H,
                         const Options& o, const std::vector<uint8_t>* mask, bool want, std::vector<float>& dxs,
                         std::vector<float>& dys, std::vector<float>& xs, std::vector<int>* ids, int bands) {
    struct Out {
        std::vector<float> dx, dy, x;
        std::vector<int> id;
    };
    std::vector<Out> out((size_t)std::max(1, bands));
    auto run = [&](int b) {
        MatchBlocks(left, right, W, H, o, mask, want, out[b].dx, out[b].dy, out[b].x, nullptr, &out[b].id, b,
                    (int)out.size());
    };
    std::vector<std::future<void>> tasks;
    for (int b = 1; b < (int)out.size(); ++b) tasks.push_back(std::async(std::launch::async, run, b));
    run(0);
    for (auto& t : tasks) t.get();
    for (const Out& b : out) {
        dxs.insert(dxs.end(), b.dx.begin(), b.dx.end());
        dys.insert(dys.end(), b.dy.begin(), b.dy.end());
        xs.insert(xs.end(), b.x.begin(), b.x.end());
        if (ids) ids->insert(ids->end(), b.id.begin(), b.id.end());
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
                // the seed is right minus left; this side's search runs the other way round
                const int dyC = SeedDy(o, W, H, bx + bs * 0.5f, by + bs * 0.5f);
                if (!MatchAt(a, b, W, H, o, bx, by, R, D, side == 0 ? -dyC : dyC, tpl, dx, dy)) continue;
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

// #269: dy = c0 + c1 u + c2 v + c3 u v, the first n terms: least squares, then Huber IRLS
// (threshold 1.5 x the residuals' MAD) so a few wrong matches do not tilt it. 4x4 normal
// equations, microseconds. ok = false when the system is singular (no spread along u or v).
struct DyFit {
    double c[4] = {};
    bool ok = false;
    double At(double u, double v) const { return c[0] + c[1] * u + c[2] * v + c[3] * u * v; }
};

DyFit FitDy(const std::vector<float>& u, const std::vector<float>& v, const std::vector<float>& dy, int n) {
    DyFit f;
    const size_t m = dy.size();
    if (m < (size_t)n + 2) return f;
    std::vector<double> w(m, 1.0), res(m);
    for (int it = 0; it < 4; ++it) {
        double A[4][5] = {};
        for (size_t i = 0; i < m; ++i) {
            const double b[4] = {1.0, u[i], v[i], (double)u[i] * v[i]};
            for (int r = 0; r < n; ++r) {
                for (int c = 0; c < n; ++c) A[r][c] += w[i] * b[r] * b[c];
                A[r][4] += w[i] * b[r] * dy[i];
            }
        }
        // Gaussian elimination, partial pivoting
        const double tiny = 1e-6 * A[0][0];
        for (int col = 0; col < n; ++col) {
            int piv = col;
            for (int r = col + 1; r < n; ++r)
                if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
            if (std::fabs(A[piv][col]) <= tiny) return DyFit{};
            if (piv != col)
                for (int c = 0; c <= 4; ++c) std::swap(A[col][c], A[piv][c]);
            for (int r = 0; r < n; ++r) {
                if (r == col) continue;
                const double k = A[r][col] / A[col][col];
                for (int c = col; c <= 4; ++c) A[r][c] -= k * A[col][c];
            }
        }
        for (int r = 0; r < 4; ++r) f.c[r] = r < n ? A[r][4] / A[r][r] : 0.0;
        f.ok = true;
        if (it == 3) break;
        for (size_t i = 0; i < m; ++i) res[i] = std::fabs(dy[i] - f.At(u[i], v[i]));
        std::vector<double> a = res;
        std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end());
        const double k = std::max(1.5 * 1.4826 * a[a.size() / 2], 0.02);  // rows
        for (size_t i = 0; i < m; ++i) w[i] = res[i] <= k ? 1.0 : k / res[i];
    }
    return f;
}

}  // namespace

void Options::SeedFrom(const Result& r, const EyePair& e) {
    seedDy0 = seedDyX = seedDyY = 0.f;
    if (!r.ok || e.PxPerRow() <= 0.f) return;
    seedDy0 = r.verticalPx / e.PxPerRow();
    seedDyX = r.dySlope;
    seedDyY = r.scaleMeasured ? r.scalePctRaw / 100.f : 0.f;
}

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
    // #269: the vertical offsets carry no scene depth, so they hold the rig's whole vertical
    // geometry: dy = ty + roll * u + zoom * v + keystone * u * v (u, v centred, -1..1). A
    // slope over x alone, as this used to fit, read a zoom mismatch as roll and vertical
    // error whenever the matches sat off-centre. The zoom needs matches spread over the
    // height, in both halves; without them the fit keeps offset + roll only.
    std::vector<float> u(xs.size()), v(xs.size());
    int top = 0, bottom = 0;
    float yMin = 1.f, yMax = 0.f;
    for (size_t i = 0; i < xs.size(); ++i) {
        u[i] = (xs[i] - 0.5f * W) / (0.5f * W);
        v[i] = (r.matchY[i] - 0.5f) * 2.f;
        top += r.matchY[i] < 0.5f;
        bottom += r.matchY[i] > 0.5f;
        yMin = std::min(yMin, r.matchY[i]);
        yMax = std::max(yMax, r.matchY[i]);
    }
    r.scaleMeasured = yMax - yMin >= 0.4f && top >= 3 && bottom >= 3;
    DyFit fit;
    if (r.scaleMeasured) {
        if (r.matches >= 12) fit = FitDy(u, v, dys, 4);
        if (!fit.ok) fit = FitDy(u, v, dys, 3);
        r.scaleMeasured = fit.ok;
    }
    if (!fit.ok) fit = FitDy(u, v, dys, 2);
    if (!fit.ok) fit = FitDy(u, v, dys, 1);
    // back to analysis px: rows per column for the roll, rows per row (unitless) for the zoom
    const double slopeA = fit.c[1] / (0.5 * W);
    // dy is in rows of H, x in columns of W; convert both to full-res DISPLAYED px before
    // the angle (a half-SBS eye is shown twice as wide as it is stored)
    const double slope = slopeA * (scaleY / e.PxPerCol());
    r.rollDeg = (float)(std::atan(slope) * 180.0 / 3.14159265358979);
    r.dySlope = (float)slopeA;
    // vertical error: the fit's offset at the picture centre, roll and zoom taken out (a
    // plain median of dy reads the offset where the matches happen to sit)
    r.verticalPx = (float)fit.c[0] * scaleY;
    r.scalePct = r.scalePctRaw = r.scaleMeasured ? (float)(fit.c[2] / (0.5 * H) * 100.0) : 0.f;
    // keystone: the u * v term at the top edge (v = -1), right side minus left
    r.keystonePx = (float)(-2.0 * fit.c[3]) * scaleY;
    {
        std::vector<float> res(dys.size());
        for (size_t i = 0; i < dys.size(); ++i) res[i] = dys[i] - (float)fit.At(u[i], v[i]);
        const float mc = median(res);
        for (float& x : res) x = std::fabs(x - mc);
        r.dyMad = median(res) * scaleY;
    }
    r.deltaE = DeltaE76(e.meanRgb[0], e.meanRgb[1]);
    r.colour = MeasureColour(e);  // #270
    r.colourDiff = MeasureColourDiff(e, median(r.parallaxPct));
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
                if (MatchAt(L, R, W, H, o, bx0 + i * o.step, by0 + j * o.step, Rx, D, 0, tpl, dx, dy))
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
        if (MatchAt(e.left, e.right, W, H, ob, bx, by, R, D, 0, tpl, dx, dy, &conf)) {
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

// Block matches re-run where an earlier pass already found each block (#281): the block at
// (bx, by) = (id % W, id / W) searched only +-R columns about that pass's dx and +-D rows
// about dyC. Same matcher and acceptance as MatchBlocks, a fraction of its search (the
// full +-8 % window is ~50 columns; a block's own neighbourhood is a few).
void MatchSeeded(const std::vector<float>& a, const std::vector<float>& b, int W, int H, const Options& o,
                 const std::vector<int>& seedIds, const std::vector<float>& seedDx, int R, int D, int dyC,
                 std::vector<float>& dys, std::vector<float>& xs, std::vector<int>* ids) {
    std::vector<float> tpl((size_t)o.block * o.block);
    for (size_t i = 0; i < seedIds.size(); ++i) {
        const int bx = seedIds[i] % W, by = seedIds[i] / W;
        float dx, dy;
        if (!MatchAt(a, b, W, H, o, bx, by, R, D, dyC, tpl, dx, dy, nullptr, (int)std::lround(seedDx[i]))) continue;
        dys.push_back(dy);
        xs.push_back((float)bx + o.block * 0.5f);
        if (ids) ids->push_back(seedIds[i]);
    }
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
    // Row bands per full-grid search (MatchBlocksParallel): up to 4, fewer on a small CPU;
    // the two candidate families below run theirs at the same time.
    const int bands = (int)std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 4u);
    // The static blocks only fit a line, so they are taken on a sparser grid than the
    // moving ones (#281: at the moving blocks' 12 px step this pass and the refine below
    // were ~0.35 s of a ~0.4 s measurement, matching ~1000 blocks twice for two numbers).
    Options os = o;
    os.step = 36;
    std::vector<int> staticIds;
    std::vector<float> staticDx;
    {
        std::vector<float> dxs, dys, xs;
        MatchBlocksParallel(cur.left, cur.right, W, H, os, &mask, false, dxs, dys, xs, &staticIds, bands);
        staticDx = dxs;
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
        std::vector<float> dys, xs;
        const std::vector<float> rw = WarpY(cur.right, W, H, rigA, rigB);
        // the same static blocks, each about its own parallax; after the warp a true
        // match sits within a pixel or two of dy = 0
        MatchSeeded(cur.left, rw, W, H, o, staticIds, staticDx, 6, 4, 0, dys, xs, nullptr);
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
        std::vector<Cand> out;
        const std::vector<uint8_t> tmask = MotionMask(tpl, tplBefore, W, H, 6.f, 3);
        std::vector<std::vector<float>> dy(n), x(n);
        std::vector<std::vector<int>> id(n);
        // The candidates are independent block-match passes over const images: one task
        // each. In series they were ~2/3 of a ~0.5 s measurement on a laptop CPU (Intel
        // PTL, Linux), which held the worker to ~2 bursts a second instead of ~4.
        //
        // #281: only the blocks the in-step candidate matched can be common to all, so the
        // earlier frames search just those, each about its in-step parallax: +-16 columns
        // holds 3 frames of 5 px/frame sideways motion at the analysis size. A block whose
        // match falls outside drops out of the common set, as one that fails to match does.
        std::vector<float> dx0;
        auto candidate = [&](int k) {
            std::vector<float> warped;
            if (rolled) warped = WarpY(searchEye(seq[n - 1 - k]), W, H, wa, wb);
            const std::vector<float>& search = rolled ? warped : searchEye(seq[n - 1 - k]);
            if (k == 0)
                MatchBlocksParallel(tpl, search, W, H, o, &tmask, true, dx0, dy[k], x[k], &id[k], bands);
            else
                MatchSeeded(tpl, search, W, H, o, id[0], dx0, 16, o.maxDy, 0, dy[k], x[k], &id[k]);
        };
        candidate(0);
        std::vector<std::future<void>> tasks;
        for (int k = 2; k < n; ++k) tasks.push_back(std::async(std::launch::async, candidate, k));
        if (n > 1) candidate(1);
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
        if (common.size() < 6) return out;
        for (int k = 0; k < n; ++k) {
            std::vector<float> v;
            for (size_t i = 0; i < id[k].size(); ++i)
                if (std::binary_search(common.begin(), common.end(), id[k][i])) v.push_back(std::fabs(dy[k][i] - expect));
            out.push_back({median(v), k, k == 0 ? 0 : late});
            if (dbg) std::fprintf(stderr, "      k=%d late=%+d: score %.3f\n", k, late, out.back().score);
        }
        return out;
    };
    // the right eye late: its content is k frames old, so it lines up with the LEFT eye
    // k frames back (warped by -rig onto the right eye's geometry)
    // The two families are independent (#281): the right-late one on its own thread.
    auto rightLate = std::async(std::launch::async, [&] {
        return family(cur.right, seq[n - 2].right, [](const EyePair& e) -> const std::vector<float>& { return e.left; },
                      +1, -rigA, -rigB);
    });
    // the left eye late: lines up with the RIGHT eye k frames back (warped by +rig)
    const std::vector<Cand> leftLate =
        family(cur.left, seq[n - 2].left, [](const EyePair& e) -> const std::vector<float>& { return e.right; }, -1, rigA, rigB);
    c = rightLate.get();  // right-late candidates first, as when the families ran in turn
    c.insert(c.end(), leftLate.begin(), leftLate.end());
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
        if (gen != seedGen_) {  // Reset() ran: the last fit and scale readings were the old input's
            seed_ = Options{};
            scaleRecent_.clear();
            colourRecent_.clear();
            seedGen_ = gen;
        }
        // #269: frames search vertically about the last fit (a zoom or roll keeps its
        // offsets inside +-maxDy at the top and bottom rows); a still has no previous fit,
        // so it is measured once plain and again seeded by its own fit.
        const bool seedFits = !seq.empty() && seq.back().eyeW == seedW_ && seq.back().h == seedH_;
        const Options o = still || !seedFits ? Options{} : seed_;
        Result r = seq.empty() ? Result{} : Measure(seq.back(), o);
        if (still && r.ok) {
            Options s;
            s.SeedFrom(r, seq.back());
            Result again = Measure(seq.back(), s);
            if (again.ok && again.matches >= r.matches) r = std::move(again);
        }
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
                    Result a = Measure(aligned, o);
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
                             "    rig: vertical %.2f px (mad %.2f) roll %.2f deg scale %+.3f%% (%s) keystone %.2f px dE %.1f "
                             "near %.2f far %.2f matches %d (aligned d=%d) serial %lld layout %d swap %d meanG %.3f/%.3f\n",
                             r.verticalPx, r.dyMad, r.rollDeg, r.scalePctRaw, r.scaleMeasured ? "measured" : "not measured",
                             r.keystonePx, r.deltaE, r.nearPct, r.farPct, r.matches, d,
                             (long long)c.serial, (int)layout, (int)swap, c.meanRgb[0][1], c.meanRgb[1][1]);
            }
        }
        // the next frame's vertical search starts from this fit; the shown scale is the
        // median of the last 5 readings (one reading moves ~0.05 % with the scene)
        if (!still) {
            seed_.SeedFrom(r, seq.empty() ? EyePair{} : seq.back());
            seedW_ = r.ok ? seq.back().eyeW : 0;
            seedH_ = r.ok ? seq.back().h : 0;
            if (r.scaleMeasured) {
                scaleRecent_.push_back(r.scalePctRaw);
                while (scaleRecent_.size() > 5) scaleRecent_.pop_front();
                std::vector<float> v(scaleRecent_.begin(), scaleRecent_.end());
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                r.scalePct = v[v.size() / 2];
            }
            // #270: the colour figures likewise: a reading moves by a few tenths of a % as
            // saturated objects cross the overlap's edges in one eye before the other, and
            // the gain alert latched those peaks. Each figure is its own median of the last 5.
            if (r.colour.measured) {
                colourRecent_.push_back(r.colour);
                while (colourRecent_.size() > 5) colourRecent_.pop_front();
                auto med = [&](auto field) {
                    std::vector<float> v;
                    for (const ColourMatch& c : colourRecent_) v.push_back(field(c));
                    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                    return v[v.size() / 2];
                };
                for (int k = 0; k < 3; ++k) {
                    r.colour.gainPct[k] = med([k](const ColourMatch& c) { return c.gainPct[k]; });
                    r.colour.liftPct[k] = med([k](const ColourMatch& c) { return c.liftPct[k]; });
                    r.colour.shape[k] = med([k](const ColourMatch& c) { return c.shape[k]; });
                }
                r.colour.lumaPct = med([](const ColourMatch& c) { return c.lumaPct; });
                r.colour.lumaStops = med([](const ColourMatch& c) { return c.lumaStops; });
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
        s.scaleMeasured = r.scaleMeasured;
        s.scalePct = r.scalePct;
        s.colourMeasured = r.colour.measured;
        for (int c = 0; c < 3; ++c) s.gainPct[c] = r.colour.gainPct[c];
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
// The alert texts, per run: near, far, vertical, colour, edges, target divergence, scale,
// colour gain (its channel letter comes first: AlertText).
const char* const kClosedFmt[9] = {
    "Foreground at %+.1f%% for %.0f s (near limit %+.1f%%)",
    "Background at %+.1f%% for %.0f s (far limit %+.1f%%)",
    "Vertical error %.1f px for %.0f s (limit %.1f px)",
    "Colour mismatch dE %.1f for %.0f s (limit %.1f)",
    "Left edge violation at %+.1f%% for %.0f s (edge limit %+.1f%%)",
    "Right edge violation at %+.1f%% for %.0f s (edge limit %+.1f%%)",
    "Background diverges on the target screen at %+.1f%% for %.0f s (limit %+.1f%%)",
    "Scale mismatch %+.2f%% for %.0f s (limit %.2f%%)",
    "Colour gain %c %+.1f%% for %.0f s (limit %.1f%%)",
};
const char* const kOpenFmt[9] = {
    "Foreground at %+.1f%% for %.0f s (now)",
    "Background at %+.1f%% for %.0f s (now)",
    "Vertical error %.1f px for %.0f s (now)",
    "Colour mismatch dE %.1f for %.0f s (now)",
    "Left edge violation at %+.1f%% for %.0f s (now)",
    "Right edge violation at %+.1f%% for %.0f s (now)",
    "Background diverges on the target screen at %+.1f%% for %.0f s (now)",
    "Scale mismatch %+.2f%% for %.0f s (now)",
    "Colour gain %c %+.1f%% for %.0f s (now)",
};
// One alert's text: the gain run (8) names its channel first.
std::string AlertText(const char* fmt, int run, int channel, float worst, double dur, float limit) {
    char b[160];
    if (run == 8) std::snprintf(b, sizeof(b), fmt, "RGB"[std::clamp(channel, 0, 2)], worst, dur, limit);
    else std::snprintf(b, sizeof(b), fmt, worst, dur, limit);
    return b;
}
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
                                     budget_.divergeFarPct, budget_.scalePct, budget_.gainPct};
        Alert a;
        a.start = r.since;
        a.end = end;
        a.what = AlertText(kClosedFmt[i], i, r.channel, r.worst, std::max(1.0, std::round(dur)), limits[i]) +
                 TargetNote(budget_, i, r.worst, targetParallaxOffsetPct_);
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
    int worstGain = 0;  // #270: the channel furthest off
    for (int c = 1; c < 3; ++c)
        if (std::fabs(s.gainPct[c]) > std::fabs(s.gainPct[worstGain])) worstGain = c;
    const bool over[kRuns] = {s.nearPct < budget_.nearPct,
                              s.farPct > budget_.farPct,
                              std::fabs(s.verticalPx) > budget_.maxVerticalPx,
                              s.deltaE > budget_.maxDeltaE,
                              s.edgeMeasured[0] && s.edgePct[0] < budget_.edgePct,
                              s.edgeMeasured[1] && s.edgePct[1] < budget_.edgePct,
                              budget_.divergeFarPct > 0.0f && shownFarPct > budget_.divergeFarPct,
                              checks_.scale && s.scaleMeasured && std::fabs(s.scalePct) > budget_.scalePct,
                              checks_.colour && s.colourMeasured && std::fabs(s.gainPct[worstGain]) > budget_.gainPct};
    const float value[kRuns] = {s.nearPct, s.farPct, std::fabs(s.verticalPx), s.deltaE, s.edgePct[0], s.edgePct[1],
                                shownFarPct, s.scalePct, s.gainPct[worstGain]};
    for (int i = 0; i < kRuns; ++i) {
        Run& r = runs_[i];
        if (over[i]) {
            if (r.since < 0) {
                r.since = s.t;
                r.worst = value[i];
                r.channel = worstGain;
                r.tc = tc;
                r.clock = WallClock(s.t);
            } else {
                const bool lowerIsWorse = i == 0 || i == 4 || i == 5;  // near and the edges: further in front is worse
                if (i == 7 || i == 8) {  // scale, gain: signed, by size
                    if (std::fabs(value[i]) > std::fabs(r.worst)) {
                        r.worst = value[i];
                        r.channel = worstGain;
                    }
                } else {
                    r.worst = lowerIsWorse ? std::min(r.worst, value[i]) : std::max(r.worst, value[i]);
                }
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

void RigMonitor::SetChecks(const Checks& c) {
    std::lock_guard<std::mutex> lk(m_);
    checks_ = c;
}

RigMonitor::Checks RigMonitor::GetChecks() const {
    std::lock_guard<std::mutex> lk(m_);
    return checks_;
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
        x.what = AlertText(kOpenFmt[i], i, r.channel, r.worst, now - r.since, 0.f) +
                 TargetNote(budget_, i, r.worst, targetParallaxOffsetPct_);
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
