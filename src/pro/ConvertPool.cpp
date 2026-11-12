// SPDX-License-Identifier: Apache-2.0
#include "pro/ConvertPool.h"

#include <algorithm>
#include <cstdlib>
#include <system_error>

namespace mp::livepix {

ConvertPool::ConvertPool(int threads) {
    const int workers = std::clamp(threads, 1, 64) - 1;
    workers_.reserve((size_t)workers);
    for (int i = 1; i <= workers; ++i) {
        try {
            workers_.emplace_back([this, i] { Loop(i); });
        } catch (const std::system_error&) {
            break;  // the system will not start more: run on the ones there are
        }
    }
}

ConvertPool::~ConvertPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void ConvertPool::RunRaw(int bands, BandFn fn, const void* ctx) {
    if (bands <= 0) return;
    const int threads = Threads();
    if (threads == 1 || bands == 1) {
        for (int b = 0; b < bands; ++b) fn(ctx, b);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        fn_ = fn;
        ctx_ = ctx;
        bands_ = bands;
        stride_ = threads;
        pending_ = std::min(bands, threads) - 1;  // the workers that have a band
        ++generation_;
    }
    wake_.notify_all();
    for (int b = 0; b < bands; b += threads) fn(ctx, b);
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return pending_ == 0; });
}

void ConvertPool::Loop(int index) {
    uint64_t seen = 0;
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        wake_.wait(lk, [&] { return quit_ || generation_ != seen; });
        if (quit_) return;
        seen = generation_;
        if (index >= bands_) continue;  // no band for this thread this time
        const BandFn fn = fn_;
        const void* ctx = ctx_;
        const int bands = bands_, stride = stride_;
        lk.unlock();
        for (int b = index; b < bands; b += stride) fn(ctx, b);
        lk.lock();
        if (--pending_ == 0) done_.notify_one();
    }
}

void BandRows(int h, int bands, int band, int& r0, int& r1) {
    h = std::max(0, h);
    bands = std::max(1, bands);
    band = std::clamp(band, 0, bands - 1);
    const int64_t pairs = h / 2;  // whole 4:2:0 chroma rows
    r0 = 2 * (int)(pairs * band / bands);
    r1 = band == bands - 1 ? h : 2 * (int)(pairs * (band + 1) / bands);
}

int BandsFor(int h, int threads) { return std::clamp(h / kMinBandRows, 1, std::max(1, threads)); }

void ToP016Banded(ConvertPool* pool, const EyeJob* eyes, int n) {
    constexpr int kMax = 8;
    if (!eyes || n <= 0) return;
    if (n > kMax) {
        ToP016Banded(pool, eyes, kMax);
        ToP016Banded(pool, eyes + kMax, n - kMax);
        return;
    }
    const int threads = pool ? pool->Threads() : 1;
    int bands[kMax], first[kMax + 1];
    first[0] = 0;
    for (int e = 0; e < n; ++e) {
        const EyeJob& j = eyes[e];
        const bool fits = j.dst && EyeFits(j.src, j.w, j.h, *j.dst, j.x0);
        // the threads shared out between the eyes: 7 for two eyes = 4 + 3
        const int share = threads / n + (e < threads % n ? 1 : 0);
        bands[e] = fits ? BandsFor(j.h, share) : 0;
        if (fits) j.dst->matrix = P016Matrix(j.fmt, j.h, j.rgbMatrix);  // before any band runs
        first[e + 1] = first[e] + bands[e];
    }
    const int total = first[n];
    auto band = [&](int b) {
        int e = 0;
        while (b >= first[e + 1]) ++e;
        const EyeJob& j = eyes[e];
        int r0 = 0, r1 = 0;
        BandRows(j.h, bands[e], b - first[e], r0, r1);
        ToP016Rows(j.fmt, j.src, j.srcRowBytes, j.w, j.h, *j.dst, j.x0, r0, r1, j.rgbMatrix);
    };
    if (!pool) {
        for (int b = 0; b < total; ++b) band(b);
        return;
    }
    pool->Run(total, band);
}

void ToP016Banded(ConvertPool* pool, RawFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, P016& dst,
                  int x0, ColorMatrix rgbMatrix) {
    EyeJob j;
    j.fmt = fmt;
    j.src = src;
    j.srcRowBytes = srcRowBytes;
    j.w = w;
    j.h = h;
    j.dst = &dst;
    j.x0 = x0;
    j.rgbMatrix = rgbMatrix;
    ToP016Banded(pool, &j, 1);
}

int ConvertThreads(int eyes, int inputEyes, unsigned hardwareThreads, const char* env) {
    if (env && *env) {
        char* end = nullptr;
        const long n = std::strtol(env, &end, 10);
        if (end != env && n >= 1) return (int)std::min(n, 64L);
    }
    const int logical = (int)std::clamp(hardwareThreads, 1u, 4096u);
    const int perEye = std::clamp(logical / (2 * std::max(1, eyes)), 1, 8);
    return perEye * std::clamp(inputEyes, 1, 2);
}

int ConvertThreads(int eyes, int inputEyes) {
    return ConvertThreads(eyes, inputEyes, std::thread::hardware_concurrency(),
                          std::getenv("MEDIAPLAYER_DECKLINK_CONVERT_THREADS"));
}

}  // namespace mp::livepix
