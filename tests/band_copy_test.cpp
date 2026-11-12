// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the renderer's upload copy in row bands (rhi/BandCopy.h): CopyRows on 1, 2,
// 3 and 7 threads (and with no pool) writes exactly what a memcpy per row writes, for packed
// planes and strided rows, odd row sizes and row counts, with band splits forced down to a
// single row; nothing outside the rows is touched; the bands tile the rows; the pool runs
// every band once; the band count and the default thread count. Then an 8K P016 frame
// (two 2160p eyes side by side, ~50 MB) copied on 1 vs N threads, printed, never failing
// (machines vary). Pure: no Vulkan, no GPU.

#include "rhi/BandCopy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using mp::bandcopy::Pool;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

const int kThreads[] = {1, 2, 3, 7};

std::vector<uint8_t> Random(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (uint8_t& b : v) {
        seed = seed * 1664525u + 1013904223u;
        b = (uint8_t)(seed >> 24);
    }
    return v;
}

// The reference: one memcpy per row into a buffer pre-filled with a marker.
std::vector<uint8_t> Reference(const std::vector<uint8_t>& src, size_t srcStride, size_t dstStride, size_t rowBytes,
                               size_t rows, size_t dstSize) {
    std::vector<uint8_t> d(dstSize, 0xA5);
    for (size_t r = 0; r < rows; ++r) std::memcpy(d.data() + r * dstStride, src.data() + r * srcStride, rowBytes);
    return d;
}

// The bands tile [0, rows) in order, each within one row of the others.
void BandSplit() {
    bool ok = true;
    for (size_t rows : {(size_t)0, (size_t)1, (size_t)2, (size_t)3, (size_t)7, (size_t)64, (size_t)1081, (size_t)2160,
                        (size_t)4321}) {
        for (int bands = 1; bands <= 9; ++bands) {
            size_t next = 0, lo = rows, hi = 0;
            for (int b = 0; b < bands; ++b) {
                size_t r0 = 0, r1 = 0;
                mp::bandcopy::BandRows(rows, bands, b, r0, r1);
                ok = ok && r0 == next && r1 >= r0;
                next = r1;
                lo = std::min(lo, r1 - r0);
                hi = std::max(hi, r1 - r0);
            }
            ok = ok && next == rows && hi - lo <= 1;
        }
    }
    CHECK(ok, "BandRows: the bands tile the rows in order, sizes within one row");
    using mp::bandcopy::BandsFor;
    using mp::bandcopy::kMinBandBytes;
    CHECK(BandsFor(0, 4) == 1 && BandsFor(kMinBandBytes - 1, 4) == 1 && BandsFor(2 * kMinBandBytes - 1, 4) == 1,
          "BandsFor: under two minimum bands is one band (on the caller)");
    CHECK(BandsFor(2 * kMinBandBytes, 4) == 2 && BandsFor(3 * kMinBandBytes, 4) == 3, "BandsFor: a band per MiB");
    CHECK(BandsFor((size_t)7680 * 2160 * 2, 4) == 4 && BandsFor((size_t)7680 * 2160 * 2, 7) == 7,
          "BandsFor: an 8K P016 luma plane gets one band per thread");
    CHECK(BandsFor((size_t)7680 * 2160 * 2, 1) == 1 && BandsFor((size_t)7680 * 2160 * 2, 0) == 1,
          "BandsFor: one thread (or none) is one band");
    CHECK(BandsFor(100, 7, 0) == 7 && BandsFor(100, 7, 1) == 7 && BandsFor(5, 7, 1) == 5,
          "BandsFor: a tiny minimum band splits small copies (tests)");
}

// The pool runs every band exactly once, bands > threads included, over and over.
void PoolRunsEveryBandOnce() {
    bool ok = true;
    for (int threads : kThreads) {
        Pool pool(threads);
        ok = ok && pool.Threads() == threads;
        for (int round = 0; round < 200; ++round)
            for (int bands : {0, 1, 2, 5, 7, 20}) {
                std::atomic<int> hits[20];
                for (auto& h : hits) h = 0;
                pool.Run(bands, [&](int b) { hits[b].fetch_add(1); });
                for (int b = 0; b < 20; ++b) ok = ok && hits[b].load() == (b < bands ? 1 : 0);
            }
    }
    CHECK(ok, "Pool::Run: every band once, for any band count, 1..7 threads");
}

// CopyRows == a memcpy per row, bit for bit, for every pool and band size.
void CopyEqualsMemcpy(std::vector<std::unique_ptr<Pool>>& pools) {
    struct Case {
        size_t rowBytes, rows, srcPad, dstPad;  // pad: stride - rowBytes
    };
    const Case cases[] = {
        {1, 1, 0, 0},        {1, 7, 0, 0},       {3, 3, 0, 0},       {7, 2, 0, 0},       {4, 1081, 0, 0},
        {3841, 3, 0, 0},     {7680 * 2, 9, 0, 0}, {1, 7, 5, 3},       {3, 1081, 13, 0},   {7, 2, 0, 9},
        {3841, 31, 64, 128}, {1920 * 4, 7, 1, 1},
    };
    for (const Case& c : cases) {
        const size_t srcStride = c.rowBytes + c.srcPad, dstStride = c.rowBytes + c.dstPad;
        // dst: the rows plus guard bytes after the last one (never written)
        const size_t dstSize = dstStride * c.rows + 64;
        const std::vector<uint8_t> src = Random(srcStride * c.rows, (uint32_t)(c.rowBytes * 31 + c.rows));
        const std::vector<uint8_t> ref = Reference(src, srcStride, dstStride, c.rowBytes, c.rows, dstSize);
        bool ok = true;
        // minimum band sizes: 1 byte (a band per thread, down to single rows), a few rows,
        // and the default (these sizes stay one band on the caller)
        for (size_t minBand : {(size_t)1, c.rowBytes * 2 + 1, mp::bandcopy::kMinBandBytes}) {
            std::vector<uint8_t> d(dstSize, 0xA5);
            mp::bandcopy::CopyRows(nullptr, d.data(), dstStride, src.data(), srcStride, c.rowBytes, c.rows, minBand);
            ok = ok && d == ref;
            for (const auto& pool : pools) {
                std::fill(d.begin(), d.end(), (uint8_t)0xA5);
                const int bands = mp::bandcopy::CopyRows(pool.get(), d.data(), dstStride, src.data(), srcStride,
                                                         c.rowBytes, c.rows, minBand);
                ok = ok && d == ref && bands >= 1 && bands <= pool->Threads() && (size_t)bands <= c.rows;
            }
        }
        char msg[160];
        std::snprintf(msg, sizeof(msg), "CopyRows %zu B x %zu rows (strides %zu/%zu): == memcpy per row on 1/2/3/7 threads",
                      c.rowBytes, c.rows, srcStride, dstStride);
        CHECK(ok, msg);
    }
    // the split really happens: 7 threads, 1-byte bands, 7+ rows -> 7 bands
    {
        const std::vector<uint8_t> src = Random(3 * 100, 9u);
        std::vector<uint8_t> d(3 * 100);
        CHECK(mp::bandcopy::CopyRows(pools.back().get(), d.data(), 3, src.data(), 3, 3, 100, 1) == 7 && d == src,
              "CopyRows: 7 threads, 100 rows, forced split: 7 bands, same bytes");
        CHECK(mp::bandcopy::CopyRows(pools.back().get(), d.data(), 3, src.data(), 3, 3, 4, 1) == 4,
              "CopyRows: never more bands than rows");
    }
    // nothing to copy: nothing written, no band
    {
        std::vector<uint8_t> d(16, 0xA5);
        const std::vector<uint8_t> s(16, 1);
        const bool none = mp::bandcopy::CopyRows(pools.back().get(), d.data(), 4, s.data(), 4, 4, 0) == 0 &&
                          mp::bandcopy::CopyRows(pools.back().get(), d.data(), 4, s.data(), 4, 0, 4) == 0 &&
                          mp::bandcopy::CopyRows(pools.back().get(), nullptr, 4, s.data(), 4, 4, 4) == 0;
        CHECK(none && d == std::vector<uint8_t>(16, 0xA5), "CopyRows: zero rows / bytes / null writes nothing");
    }
}

// A big plane at the default band size: split one band per thread, same bytes.
void BigPlane(std::vector<std::unique_ptr<Pool>>& pools) {
    const size_t rowBytes = 7680 * 2, rows = 2160;  // the 8K P016 luma plane
    const std::vector<uint8_t> src = Random(rowBytes * rows, 77u);
    std::vector<uint8_t> d(rowBytes * rows);
    bool ok = true;
    for (const auto& pool : pools) {
        std::fill(d.begin(), d.end(), (uint8_t)0);
        const int bands = mp::bandcopy::CopyRows(pool.get(), d.data(), rowBytes, src.data(), rowBytes, rowBytes, rows);
        ok = ok && d == src && bands == pool->Threads();
    }
    CHECK(ok, "8K P016 luma (33 MB): one band per thread (1/2/3/7), same bytes");
}

void ThreadCounts() {
    using mp::bandcopy::UploadThreads;
    CHECK(UploadThreads(64, nullptr) == 4, "64 logical: 4 copy threads");
    CHECK(UploadThreads(16, nullptr) == 4 && UploadThreads(8, nullptr) == 2, "16 logical: 4; 8 logical: 2");
    CHECK(UploadThreads(4, nullptr) == 1 && UploadThreads(2, nullptr) == 1 && UploadThreads(0, nullptr) == 1,
          "4 or fewer (or unknown): 1, the render thread alone");
    CHECK(UploadThreads(64, "1") == 1 && UploadThreads(4, "8") == 8, "MEDIAPLAYER_UPLOAD_THREADS sets it");
    CHECK(UploadThreads(64, "100") == 16, "MEDIAPLAYER_UPLOAD_THREADS capped at 16");
    CHECK(UploadThreads(64, "0") == 4 && UploadThreads(64, "x") == 4 && UploadThreads(64, "") == 4,
          "0 / not a number / empty: the default");
}

double MedianMs(const std::function<void()>& f, int runs) {
    std::vector<double> ms;
    for (int i = 0; i < runs; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

// Informational: the 8K P016 frame (Y 33 MB + UV 16.6 MB) on 1 vs N threads. Ordinary
// memory here; the renderer's staging is host-visible (often write-combined) memory.
void Timing() {
    const size_t yBytes = (size_t)7680 * 2160 * 2, uvBytes = (size_t)7680 * 1080 * 2;
    const std::vector<uint8_t> y = Random(yBytes, 1u), uv = Random(uvBytes, 2u);
    std::vector<uint8_t> dy(yBytes), duv(uvBytes);
    const unsigned hw = std::thread::hardware_concurrency();
    const int def = mp::bandcopy::UploadThreads(hw, nullptr);
    std::printf("  timing, 7680x2160 P016 (Y + UV, %.1f MB), median of 9 (%u logical; default %d thread(s)):\n",
                (yBytes + uvBytes) / (1024.0 * 1024.0), hw, def);
    double one = 0.0;
    for (int t : {1, 2, 4, 8}) {
        if (t > 1 && hw && (unsigned)t > hw) continue;
        Pool pool(t);
        const double ms = MedianMs(
            [&] {
                mp::bandcopy::CopyRows(&pool, dy.data(), 7680 * 2, y.data(), 7680 * 2, 7680 * 2, 2160);
                mp::bandcopy::CopyRows(&pool, duv.data(), 7680 * 2, uv.data(), 7680 * 2, 7680 * 2, 1080);
            },
            9);
        if (t == 1) one = ms;
        std::printf("    %d thread(s): %6.2f ms  (x%.1f)%s\n", t, ms, one > 0 ? one / ms : 1.0,
                    t == def ? "  <- default" : "");
    }
}

}  // namespace

int main() {
    BandSplit();
    PoolRunsEveryBandOnce();
    std::vector<std::unique_ptr<Pool>> pools;  // persistent, as the renderer's: reused across sizes
    for (int t : kThreads) pools.push_back(std::make_unique<Pool>(t));
    CopyEqualsMemcpy(pools);
    BigPlane(pools);
    ThreadCounts();
    Timing();
    if (g_failures) {
        std::fprintf(stderr, "band_copy_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("band_copy_test: all passed\n");
    return 0;
}
