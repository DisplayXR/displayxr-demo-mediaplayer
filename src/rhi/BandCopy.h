// SPDX-License-Identifier: Apache-2.0
//
// BandCopy - a big copy split into row bands over a small persistent worker pool. The
// render thread's per-frame staging copy is the case it is for: Media Player Pro showing two
// 2160p59.94 DeckLink eyes side by side uploads a 7680x2160 P016 frame (~50 MB) every frame,
// and one thread copying it into mapped staging held the render thread to ~45-50 fps.
// The bands are disjoint rows, so the result is a memcpy per row, bit for bit. Small copies
// (one band's worth) run on the calling thread and never wake the pool.
//
// Trunk code (the base player's uploads use it too), so it depends on nothing in src/pro.
// Pure (no Vulkan): tests/band_copy_test.cpp.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace mp::bandcopy {

class Pool {
public:
    // `threads` in all, the calling thread included: 1 = no workers, everything on the
    // caller. Fewer if the system will not start them (Threads() says).
    explicit Pool(int threads);
    ~Pool();  // joins the workers; no Run() may be in progress
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    int Threads() const { return 1 + (int)workers_.size(); }

    // fn(band) once for every band in [0, bands): band b on thread b % Threads(), thread 0
    // being the caller. Returns when all are done. One caller at a time.
    template <class F>
    void Run(int bands, const F& fn) {
        RunRaw(bands, [](const void* f, int band) { (*static_cast<const F*>(f))(band); }, &fn);
    }

private:
    using BandFn = void (*)(const void*, int);
    void RunRaw(int bands, BandFn fn, const void* ctx);
    void Loop(int index);

    std::mutex m_;
    std::condition_variable wake_, done_;
    BandFn fn_ = nullptr;
    const void* ctx_ = nullptr;
    int bands_ = 0, stride_ = 1, pending_ = 0;
    uint64_t generation_ = 0;
    bool quit_ = false;
    std::vector<std::thread> workers_;
};

// A band smaller than this is not worth waking a thread for (a wake costs tens of
// microseconds; a MiB copies in about a tenth of a millisecond).
constexpr size_t kMinBandBytes = size_t(1) << 20;

// How many bands a copy of `bytes` gets on `threads`: one per thread, each at least
// `minBandBytes`, so anything under two of them is one band on the caller.
int BandsFor(size_t bytes, int threads, size_t minBandBytes = kMinBandBytes);

// Band `band` of `bands` over `rows` rows: [r0, r1). The bands tile [0, rows) in order and
// differ in size by at most one row.
void BandRows(size_t rows, int bands, int band, size_t& r0, size_t& r1);

// Copy `rows` rows of `rowBytes` bytes, rows `srcStride` / `dstStride` bytes apart (a
// tightly packed plane: both == rowBytes), in row bands over `pool` (nullptr = this thread).
// The same bytes as a memcpy per row; nothing outside the rows is written. Returns the
// number of bands it used (1 = it ran on the caller only).
int CopyRows(Pool* pool, void* dst, size_t dstStride, const void* src, size_t srcStride, size_t rowBytes, size_t rows,
             size_t minBandBytes = kMinBandBytes);

// The render thread's copy threads, its own included. `env` is MEDIAPLAYER_UPLOAD_THREADS:
// a number >= 1 sets them (at most 16; 1 = every copy on the render thread, as before).
// Otherwise a quarter of the logical processors, 1 to 4: a copy into write-combined memory
// stops scaling at a few threads, and the capture converters want the rest.
// hardwareThreads 0 (unknown) counts as 1.
int UploadThreads(unsigned hardwareThreads, const char* env);
// The same from std::thread::hardware_concurrency() and the environment.
int UploadThreads();

}  // namespace mp::bandcopy
