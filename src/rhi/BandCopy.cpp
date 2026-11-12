// SPDX-License-Identifier: Apache-2.0
#include "rhi/BandCopy.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace mp::bandcopy {

Pool::Pool(int threads) {
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

Pool::~Pool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void Pool::RunRaw(int bands, BandFn fn, const void* ctx) {
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

void Pool::Loop(int index) {
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

int BandsFor(size_t bytes, int threads, size_t minBandBytes) {
    const size_t perBand = std::max<size_t>(1, minBandBytes);
    const size_t bands = std::max<size_t>(1, bytes / perBand);
    return (int)std::min<size_t>(bands, (size_t)std::max(1, threads));
}

void BandRows(size_t rows, int bands, int band, size_t& r0, size_t& r1) {
    bands = std::max(1, bands);
    band = std::clamp(band, 0, bands - 1);
    // rows * band never overflows for any real plane (rows < 2^32, bands <= 64)
    r0 = rows * (size_t)band / (size_t)bands;
    r1 = band == bands - 1 ? rows : rows * (size_t)(band + 1) / (size_t)bands;
}

namespace {

void CopyBand(uint8_t* dst, size_t dstStride, const uint8_t* src, size_t srcStride, size_t rowBytes, size_t r0,
              size_t r1) {
    if (r1 <= r0 || rowBytes == 0) return;
    if (dstStride == rowBytes && srcStride == rowBytes) {  // packed: the band is one block
        std::memcpy(dst + r0 * rowBytes, src + r0 * rowBytes, (r1 - r0) * rowBytes);
        return;
    }
    for (size_t r = r0; r < r1; ++r) std::memcpy(dst + r * dstStride, src + r * srcStride, rowBytes);
}

}  // namespace

int CopyRows(Pool* pool, void* dst, size_t dstStride, const void* src, size_t srcStride, size_t rowBytes, size_t rows,
             size_t minBandBytes) {
    if (!dst || !src || rows == 0 || rowBytes == 0) return 0;
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    const int threads = pool ? pool->Threads() : 1;
    const int bands = (int)std::min<size_t>((size_t)BandsFor(rowBytes * rows, threads, minBandBytes), rows);
    if (bands <= 1 || !pool) {
        CopyBand(d, dstStride, s, srcStride, rowBytes, 0, rows);
        return 1;
    }
    pool->Run(bands, [&](int band) {
        size_t r0 = 0, r1 = 0;
        BandRows(rows, bands, band, r0, r1);
        CopyBand(d, dstStride, s, srcStride, rowBytes, r0, r1);
    });
    return bands;
}

int UploadThreads(unsigned hardwareThreads, const char* env) {
    if (env && *env) {
        char* end = nullptr;
        const long n = std::strtol(env, &end, 10);
        if (end != env && n >= 1) return (int)std::min(n, 16L);
    }
    const int logical = (int)std::clamp(hardwareThreads, 1u, 4096u);
    return std::clamp(logical / 4, 1, 4);
}

int UploadThreads() {
    return UploadThreads(std::thread::hardware_concurrency(), std::getenv("MEDIAPLAYER_UPLOAD_THREADS"));
}

}  // namespace mp::bandcopy
