// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the capture convert in row bands (Media Player Pro, pro/ConvertPool.h):
// every raw format (UYVY, v210, BGRA, r210) converted on 1, 2, 3 and 7 threads equals the
// one-pass convert bit for bit, at 4K, HD and odd sizes, for one eye and for a dual-stream
// input's two eyes converted together into one side-by-side frame; the band split keeps
// 4:2:0 chroma rows whole; the pool runs every band once; the ring publish in row bands (a
// two-input pair composed side by side, one frame as it is) equals the one-pass copy; the
// default thread counts. Then a 4K v210 timing and a 4K pair's compose, 1 vs N threads,
// which are printed and never fail (machines vary).
// Pure: no SDK, no card, no GPU.

#include "pro/ConvertPool.h"
#include "pro/LivePixels.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using mp::ColorMatrix;
using mp::livepix::ConvertPool;
using mp::livepix::EyeJob;
using mp::livepix::P016;
using mp::livepix::RawFormat;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

const RawFormat kFormats[] = {RawFormat::Uyvy, RawFormat::V210, RawFormat::Bgra, RawFormat::R210};
const int kThreads[] = {1, 2, 3, 7};

const char* Name(RawFormat f) {
    switch (f) {
        case RawFormat::Uyvy: return "UYVY";
        case RawFormat::V210: return "v210";
        case RawFormat::Bgra: return "BGRA";
        case RawFormat::R210: return "r210";
    }
    return "?";
}

// Row bytes as the card lays them out (v210: 48-pixel blocks of 128 bytes; r210: 64-pixel
// blocks of 256), UYVY / BGRA with some padding so a row's end is not the next row's start.
int RowBytes(RawFormat f, int w) {
    switch (f) {
        case RawFormat::Uyvy: return 2 * (w + 1) + 64;
        case RawFormat::V210: return ((w + 47) / 48) * 128;
        case RawFormat::Bgra: return 4 * w + 64;
        case RawFormat::R210: return ((w + 63) / 64) * 256;
    }
    return 0;
}

struct Raw {
    RawFormat fmt;
    int w, h, rowBytes;
    std::vector<uint8_t> bytes;
};

Raw RandomFrame(RawFormat f, int w, int h, uint32_t seed) {
    Raw r{f, w, h, RowBytes(f, w), {}};
    r.bytes.resize((size_t)r.rowBytes * h);
    for (uint8_t& b : r.bytes) {
        seed = seed * 1664525u + 1013904223u;
        b = (uint8_t)(seed >> 24);
    }
    return r;
}

EyeJob JobOf(const Raw& r, P016& dst, int x0, ColorMatrix m) {
    EyeJob j;
    j.fmt = r.fmt;
    j.src = r.bytes.data();
    j.srcRowBytes = r.rowBytes;
    j.w = r.w;
    j.h = r.h;
    j.dst = &dst;
    j.x0 = x0;
    j.rgbMatrix = m;
    return j;
}

// A frame w x h pre-filled with a marker, so samples a convert must not touch are compared too.
void Blank(P016& d, int w, int h) {
    d.Resize(w, h);
    std::fill(d.y.begin(), d.y.end(), (uint16_t)0x1234);
    std::fill(d.uv.begin(), d.uv.end(), (uint16_t)0x4321);
    d.matrix = ColorMatrix::BT2020;  // neither SDI rule: shows that the tag was set
}

bool SamePixels(const P016& a, const P016& b) {
    return a.width == b.width && a.height == b.height && a.y == b.y && a.uv == b.uv;
}
bool Same(const P016& a, const P016& b) { return SamePixels(a, b) && a.matrix == b.matrix; }

// The bands tile [0, h) in order, start on even rows, end on even rows or h.
void BandSplit() {
    bool ok = true;
    for (int h : {0, 1, 2, 3, 4, 5, 7, 64, 130, 722, 723, 1080, 2160})
        for (int bands = 1; bands <= 9; ++bands) {
            int next = 0;
            for (int b = 0; b < bands; ++b) {
                int r0 = -1, r1 = -1;
                mp::livepix::BandRows(h, bands, b, r0, r1);
                ok = ok && r0 == next && r1 >= r0 && (r0 % 2) == 0 && ((r1 % 2) == 0 || r1 == h);
                if (h >= 2 * bands) ok = ok && r1 > r0;  // enough row pairs: no empty band
                next = r1;
            }
            ok = ok && next == h;
        }
    CHECK(ok, "bands tile the rows, on whole 4:2:0 chroma rows");
    CHECK(mp::livepix::BandsFor(2160, 8) == 8 && mp::livepix::BandsFor(2160, 1) == 1, "BandsFor: one per thread");
    CHECK(mp::livepix::BandsFor(130, 7) == 2 && mp::livepix::BandsFor(40, 7) == 1, "BandsFor: >= 64 rows a band");
}

// ToP016Rows: any split at an even row adds up to the one-pass convert, it never touches the
// tags, and a band that would cut a chroma row in half is refused (nothing written).
void RowsAddUp() {
    bool ok = true, tagsOk = true, refusedOk = true;
    for (RawFormat f : kFormats)
        for (int w : {14, 15})
            for (int h : {11, 12}) {
                const Raw raw = RandomFrame(f, w, h, 77u + (uint32_t)w * 13u + (uint32_t)h);
                P016 ref, d;
                Blank(ref, w + 6, h);
                mp::livepix::ToP016(f, raw.bytes.data(), raw.rowBytes, w, h, ref, 6, ColorMatrix::BT709);
                tagsOk = tagsOk && ref.matrix == mp::livepix::P016Matrix(f, h, ColorMatrix::BT709);
                for (int s = 0; s <= h; s += 2) {
                    Blank(d, w + 6, h);
                    ok = ok && mp::livepix::ToP016Rows(f, raw.bytes.data(), raw.rowBytes, w, h, d, 6, 0, s, ColorMatrix::BT709);
                    ok = ok && mp::livepix::ToP016Rows(f, raw.bytes.data(), raw.rowBytes, w, h, d, 6, s, h, ColorMatrix::BT709);
                    ok = ok && SamePixels(d, ref);
                    tagsOk = tagsOk && d.matrix == ColorMatrix::BT2020;  // Blank's: the bands leave it
                }
                // 2-row bands, last one first
                Blank(d, w + 6, h);
                for (int r0 = (h - 1) & ~1; r0 >= 0; r0 -= 2)
                    ok = ok && mp::livepix::ToP016Rows(f, raw.bytes.data(), raw.rowBytes, w, h, d, 6, r0,
                                                       std::min(r0 + 2, h), ColorMatrix::BT709);
                ok = ok && SamePixels(d, ref);
                // refused: odd start, odd end short of h, past h, backwards
                P016 blank;
                Blank(d, w + 6, h);
                Blank(blank, w + 6, h);
                const int bad[][2] = {{1, 4}, {0, 3}, {0, h + 1}, {4, 2}, {-2, 2}};
                for (const auto& b : bad)
                    refusedOk = refusedOk && !mp::livepix::ToP016Rows(f, raw.bytes.data(), raw.rowBytes, w, h, d, 6,
                                                                      b[0], b[1], ColorMatrix::BT709);
                refusedOk = refusedOk && Same(d, blank);
            }
    CHECK(ok, "row bands at every even split add up to the one-pass convert (all formats, odd w / h)");
    CHECK(tagsOk, "ToP016 sets the tag, the row bands never touch it");
    CHECK(refusedOk, "a band that splits a chroma row or leaves the eye is refused and writes nothing");
}

// The pool runs every band exactly once, bands > threads included, over and over.
void PoolRunsEveryBandOnce() {
    bool ok = true;
    for (int threads : {1, 2, 3, 7}) {
        ConvertPool pool(threads);
        ok = ok && pool.Threads() == threads;
        for (int round = 0; round < 200; ++round)
            for (int bands : {0, 1, 2, 5, 7, 20}) {
                std::atomic<int> hits[20];
                for (auto& h : hits) h = 0;
                pool.Run(bands, [&](int b) { hits[b].fetch_add(1); });
                for (int b = 0; b < 20; ++b) ok = ok && hits[b].load() == (b < bands ? 1 : 0);
            }
    }
    CHECK(ok, "ConvertPool::Run: every band once, for any band count, 1..7 threads");
}

// One eye: banded on 1, 2, 3 and 7 threads == one pass, every format and size.
void BandedEqualsOnePass(std::vector<std::unique_ptr<ConvertPool>>& pools) {
    const int sizes[][2] = {{3840, 2160}, {1920, 1080}, {1282, 722}, {1283, 723}};
    for (const auto& sz : sizes) {
        const int w = sz[0], h = sz[1];
        for (RawFormat f : kFormats) {
            const ColorMatrix m = f == RawFormat::R210 ? ColorMatrix::BT2020 : ColorMatrix::BT709;
            const Raw raw = RandomFrame(f, w, h, 1000u + (uint32_t)w + (uint32_t)f);
            P016 ref, d;
            Blank(ref, w + 6, h);
            mp::livepix::ToP016(f, raw.bytes.data(), raw.rowBytes, w, h, ref, 6, m);
            bool ok = true;
            for (const auto& pool : pools) {
                Blank(d, w + 6, h);
                mp::livepix::ToP016Banded(pool.get(), f, raw.bytes.data(), raw.rowBytes, w, h, d, 6, m);
                ok = ok && Same(d, ref);
            }
            Blank(d, w + 6, h);
            mp::livepix::ToP016Banded(nullptr, f, raw.bytes.data(), raw.rowBytes, w, h, d, 6, m);
            ok = ok && Same(d, ref);
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s %dx%d: banded on 1/2/3/7 threads == one pass, bit for bit", Name(f), w, h);
            CHECK(ok, msg);
        }
    }
    // (and the 4K bands really are split: one per thread)
    CHECK(mp::livepix::BandsFor(2160, pools.back()->Threads()) == 7, "4K on 7 threads: 7 bands");
}

// Dual-stream 3D: the left and right eyes converted together into one side-by-side frame
// == ToP016 left, then right, on one thread.
void DualStreamEqualsOnePass(std::vector<std::unique_ptr<ConvertPool>>& pools) {
    struct Case {
        RawFormat f;
        int w, h;
    };
    const Case cases[] = {{RawFormat::V210, 3840, 2160}, {RawFormat::V210, 1920, 1080}, {RawFormat::Uyvy, 1282, 722},
                          {RawFormat::Bgra, 1282, 722},  {RawFormat::R210, 1282, 722},  {RawFormat::V210, 1282, 722}};
    for (const Case& c : cases) {
        const Raw left = RandomFrame(c.f, c.w, c.h, 5u), right = RandomFrame(c.f, c.w, c.h, 6u);
        P016 ref, d;
        Blank(ref, 2 * c.w, c.h);
        mp::livepix::ToP016(c.f, left.bytes.data(), left.rowBytes, c.w, c.h, ref, 0, ColorMatrix::BT709);
        mp::livepix::ToP016(c.f, right.bytes.data(), right.rowBytes, c.w, c.h, ref, c.w, ColorMatrix::BT709);
        bool ok = true;
        for (const auto& pool : pools) {
            Blank(d, 2 * c.w, c.h);
            const EyeJob jobs[2] = {JobOf(left, d, 0, ColorMatrix::BT709), JobOf(right, d, c.w, ColorMatrix::BT709)};
            mp::livepix::ToP016Banded(pool.get(), jobs, 2);
            ok = ok && Same(d, ref);
        }
        Blank(d, 2 * c.w, c.h);
        const EyeJob jobs[2] = {JobOf(left, d, 0, ColorMatrix::BT709), JobOf(right, d, c.w, ColorMatrix::BT709)};
        mp::livepix::ToP016Banded(nullptr, jobs, 2);
        ok = ok && Same(d, ref);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "dual-stream %s %dx%d x2: both eyes banded together == one pass each", Name(c.f),
                      c.w, c.h);
        CHECK(ok, msg);
    }
    // a right eye that does not fit is refused as ToP016 refuses it; the left still converts
    const Raw left = RandomFrame(RawFormat::V210, 1282, 722, 7u), big = RandomFrame(RawFormat::V210, 1300, 722, 8u);
    P016 ref, d;
    Blank(ref, 2 * 1282, 722);
    mp::livepix::ToP016(RawFormat::V210, left.bytes.data(), left.rowBytes, 1282, 722, ref, 0, ColorMatrix::BT709);
    mp::livepix::ToP016(RawFormat::V210, big.bytes.data(), big.rowBytes, 1300, 722, ref, 1282, ColorMatrix::BT709);
    Blank(d, 2 * 1282, 722);
    const EyeJob jobs[2] = {JobOf(left, d, 0, ColorMatrix::BT709), JobOf(big, d, 1282, ColorMatrix::BT709)};
    mp::livepix::ToP016Banded(pools.back().get(), jobs, 2);
    CHECK(Same(d, ref), "dual-stream: a right eye too wide for the frame is skipped, as one pass skips it");
}

// A P016 frame w x h of random samples.
P016 RandomP016(int w, int h, uint32_t seed) {
    P016 p;
    p.Resize(w, h);
    for (uint16_t& s : p.y) {
        seed = seed * 1664525u + 1013904223u;
        s = (uint16_t)(seed >> 16);
    }
    for (uint16_t& s : p.uv) {
        seed = seed * 1664525u + 1013904223u;
        s = (uint16_t)(seed >> 16);
    }
    return p;
}

// The ring publish: two eyes side by side (two-input stereo) and one frame as it is, in row
// bands == the one-pass loop LiveSourceBase used, bit for bit; nothing past the frame written.
void PublishEqualsOnePass(std::vector<std::unique_ptr<ConvertPool>>& pools) {
    struct Case {
        int lw, lh, rw, rh;
    };
    // equal 4K eyes; HD; odd widths; eyes of different sizes (the smaller one is used);
    // an odd height (the last row is dropped); a 2-row frame (one band)
    const Case cases[] = {{3840, 2160, 3840, 2160}, {1920, 1080, 1920, 1080}, {1283, 723, 1283, 723},
                          {1290, 722, 1282, 730},   {641, 131, 643, 129},     {7, 2, 9, 3}};
    for (const Case& c : cases) {
        const P016 l = RandomP016(c.lw, c.lh, 21u + (uint32_t)c.lw), r = RandomP016(c.rw, c.rh, 22u + (uint32_t)c.rw);
        const int cw = std::min(c.lw, c.rw), ch = std::min(c.lh, c.rh) & ~1, w = 2 * cw;
        const size_t ys = (size_t)w * ch, uvs = (size_t)w * (ch / 2);
        // reference: LiveSourceBase::PublishPair's loop before the bands; 8 guard samples after each plane
        std::vector<uint16_t> refY(ys + 8, 0x5A5A), refUv(uvs + 8, 0x5A5A);
        for (int row = 0; row < ch; ++row) {
            std::memcpy(refY.data() + (size_t)row * w, l.y.data() + (size_t)row * l.width, (size_t)cw * 2);
            std::memcpy(refY.data() + (size_t)row * w + cw, r.y.data() + (size_t)row * r.width, (size_t)cw * 2);
        }
        for (int row = 0; row < ch / 2; ++row) {
            std::memcpy(refUv.data() + (size_t)row * w, l.uv.data() + (size_t)row * l.width, (size_t)cw * 2);
            std::memcpy(refUv.data() + (size_t)row * w + cw, r.uv.data() + (size_t)row * r.width, (size_t)cw * 2);
        }
        bool ok = true;
        std::vector<uint16_t> y(ys + 8), uv(uvs + 8);
        auto run = [&](ConvertPool* pool) {
            std::fill(y.begin(), y.end(), (uint16_t)0x5A5A);
            std::fill(uv.begin(), uv.end(), (uint16_t)0x5A5A);
            mp::livepix::ComposePairBanded(pool, l, r, cw, ch, y.data(), uv.data());
            ok = ok && y == refY && uv == refUv;
        };
        run(nullptr);
        for (const auto& pool : pools) run(pool.get());
        char msg[160];
        std::snprintf(msg, sizeof(msg), "pair %dx%d + %dx%d side by side: banded on 1/2/3/7 threads == one pass", c.lw,
                      c.lh, c.rw, c.rh);
        CHECK(ok, msg);

        // one frame as it is (Publish): the left eye's planes
        bool same = true;
        std::vector<uint16_t> fy(l.y.size() + 8), fuv(l.uv.size() + 8);
        std::vector<uint16_t> wantY(l.y), wantUv(l.uv);
        wantY.resize(fy.size(), 0x5A5A);
        wantUv.resize(fuv.size(), 0x5A5A);
        auto runOne = [&](ConvertPool* pool) {
            std::fill(fy.begin(), fy.end(), (uint16_t)0x5A5A);
            std::fill(fuv.begin(), fuv.end(), (uint16_t)0x5A5A);
            mp::livepix::CopyFrameBanded(pool, l, fy.data(), fuv.data());
            same = same && fy == wantY && fuv == wantUv;
        };
        runOne(nullptr);
        for (const auto& pool : pools) runOne(pool.get());
        std::snprintf(msg, sizeof(msg), "frame %dx%d: copied in bands on 1/2/3/7 threads == the planes", c.lw, c.lh);
        CHECK(same, msg);
    }
    // a frame whose planes do not match its size (not Resize()d) is still copied as it is
    P016 odd;
    odd.width = 4;
    odd.height = 4;
    odd.y.assign(10, 7);
    odd.uv.assign(3, 9);
    std::vector<uint16_t> oy(10, 0), ouv(3, 0);
    mp::livepix::CopyFrameBanded(pools.back().get(), odd, oy.data(), ouv.data());
    CHECK(oy == odd.y && ouv == odd.uv, "a frame not Resize()d: its planes copied as they are");
}

void ThreadCounts() {
    using mp::livepix::ConvertThreads;
    // two inputs (Pair): one eye each
    CHECK(ConvertThreads(2, 1, 64, nullptr) == 8, "64 logical, two inputs: 8 per input");
    CHECK(ConvertThreads(2, 1, 128, nullptr) == 8, "128 logical: still 8 per eye");
    CHECK(ConvertThreads(2, 1, 16, nullptr) == 4, "16 logical, two inputs: 4 each (half the box)");
    CHECK(ConvertThreads(2, 1, 8, nullptr) == 2, "8 logical, two inputs: 2 each");
    CHECK(ConvertThreads(2, 1, 2, nullptr) == 1 && ConvertThreads(2, 1, 0, nullptr) == 1, "small / unknown: 1");
    // one dual-stream input: two eyes on one lane
    CHECK(ConvertThreads(2, 2, 64, nullptr) == 16, "64 logical, one dual-stream input: 8 per eye = 16");
    CHECK(ConvertThreads(2, 2, 8, nullptr) == 4, "8 logical, one dual-stream input: 2 per eye = 4");
    // one input, one eye
    CHECK(ConvertThreads(1, 1, 64, nullptr) == 8, "64 logical, one eye: 8");
    CHECK(ConvertThreads(1, 1, 8, nullptr) == 4, "8 logical, one eye: 4");
    // MEDIAPLAYER_DECKLINK_CONVERT_THREADS: threads per input, 1 = no workers
    CHECK(ConvertThreads(2, 1, 64, "1") == 1 && ConvertThreads(2, 2, 64, "1") == 1, "env 1: one thread per input");
    CHECK(ConvertThreads(2, 2, 64, "12") == 12, "env 12: 12 per input");
    CHECK(ConvertThreads(2, 1, 64, "500") == 64, "env capped at 64");
    CHECK(ConvertThreads(2, 1, 64, "0") == 8 && ConvertThreads(2, 1, 64, "x") == 8 && ConvertThreads(2, 1, 64, "") == 8,
          "env 0 / not a number / empty: the default");
}

template <class F>
double MedianMs(const F& fn, int runs) {
    fn();  // warm-up: page in the frames
    std::vector<double> ms;
    for (int i = 0; i < runs; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

// Informational: a 2160p v210 eye, and a dual-stream pair of them, on 1 vs N threads.
void Timing() {
    const int w = 3840, h = 2160;
    const Raw left = RandomFrame(RawFormat::V210, w, h, 11u), right = RandomFrame(RawFormat::V210, w, h, 12u);
    const unsigned hw = std::thread::hardware_concurrency();
    std::printf("  timing, 3840x2160 v210 -> P016 (median of 15; %u logical processors; 59.94 fps = 16.7 ms):\n", hw);
    std::vector<int> counts = {1, 2, 4, 8};
    const int def = mp::livepix::ConvertThreads(2, 1);
    if (std::find(counts.begin(), counts.end(), def) == counts.end()) counts.push_back(def);
    P016 d;
    d.Resize(w, h);
    double one = 0.0;
    for (int t : counts) {
        if (t > 1 && hw && (unsigned)t > hw) continue;
        ConvertPool pool(t);
        const double ms = MedianMs(
            [&] {
                mp::livepix::ToP016Banded(&pool, RawFormat::V210, left.bytes.data(), left.rowBytes, w, h, d, 0,
                                          ColorMatrix::BT709);
            },
            15);
        if (t == 1) one = ms;
        std::printf("    one eye,  %2d thread(s): %6.2f ms  (x%.1f)%s\n", t, ms, one > 0 ? one / ms : 1.0,
                    t == def ? "  <- default per input, two inputs" : "");
    }
    P016 sbs;
    sbs.Resize(2 * w, h);
    const EyeJob jobs[2] = {JobOf(left, sbs, 0, ColorMatrix::BT709), JobOf(right, sbs, w, ColorMatrix::BT709)};
    const int dualDef = mp::livepix::ConvertThreads(2, 2);
    double dualOne = 0.0;
    for (int t : {1, dualDef}) {
        ConvertPool pool(t);
        const double ms = MedianMs([&] { mp::livepix::ToP016Banded(&pool, jobs, 2); }, 15);
        if (t == 1) dualOne = ms;
        std::printf("    dual-stream (two eyes), %2d thread(s): %6.2f ms  (x%.1f)%s\n", t, ms,
                    dualOne > 0 ? dualOne / ms : 1.0, t == dualDef && t > 1 ? "  <- default, one dual-stream input" : "");
        if (dualDef == 1) break;
    }
    // The two-input pair composed side by side into the ring (PublishPair, ~50 MB).
    const P016 le = RandomP016(w, h, 31u), re = RandomP016(w, h, 32u);
    std::vector<uint16_t> py((size_t)2 * w * h), puv((size_t)2 * w * (h / 2));
    double pairOne = 0.0;
    for (int t : {1, def}) {
        ConvertPool pool(t);
        const double ms =
            MedianMs([&] { mp::livepix::ComposePairBanded(&pool, le, re, w, h, py.data(), puv.data()); }, 15);
        if (t == 1) pairOne = ms;
        std::printf("    pair compose 7680x2160, %2d thread(s): %6.2f ms  (x%.1f)%s\n", t, ms,
                    pairOne > 0 ? pairOne / ms : 1.0, t == def && t > 1 ? "  <- default per input" : "");
        if (def == 1) break;
    }
}

}  // namespace

int main() {
    BandSplit();
    RowsAddUp();
    PoolRunsEveryBandOnce();
    std::vector<std::unique_ptr<ConvertPool>> pools;  // persistent, as a capture's: reused across formats and sizes
    for (int t : kThreads) pools.push_back(std::make_unique<ConvertPool>(t));
    BandedEqualsOnePass(pools);
    DualStreamEqualsOnePass(pools);
    PublishEqualsOnePass(pools);
    ThreadCounts();
    Timing();
    if (g_failures) {
        std::fprintf(stderr, "live_convert_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("live_convert_test: all passed\n");
    return 0;
}
