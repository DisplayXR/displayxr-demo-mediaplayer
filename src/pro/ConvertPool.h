// SPDX-License-Identifier: Apache-2.0
//
// Row-band conversion for live capture. A 2160p59.94 10-bit eye is ~22 MB of v210 to
// unpack every 16.7 ms. Converted on one thread per input, two 4K60 DeckLink inputs reached
// ~40 fps while the 64-core station sat at 12 % CPU. Each capture input now owns a
// ConvertPool: a few persistent worker threads that split every eye into horizontal row
// bands (livepix::ToP016Rows). The bands write disjoint samples, so the frame is the one-pass
// conversion bit for bit. A dual-stream 3D input converts its two eyes at once, the pool
// shared between them. Pure (no SDK): tests/live_convert_test.cpp.
#pragma once

#include "pro/LivePixels.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace mp::livepix {

class ConvertPool {
public:
    // `threads` in all, the calling thread included: 1 = no workers, everything on the
    // caller. Fewer if the system will not start them (Threads() says).
    // `pinGroup` >= 0: each worker is pinned to that last-level-cache group (platform/ThreadPin.h;
    // the caller pins itself). -1, the default: not pinned.
    explicit ConvertPool(int threads, int pinGroup = -1);
    ~ConvertPool();  // joins the workers; no Run() may be in progress
    ConvertPool(const ConvertPool&) = delete;
    ConvertPool& operator=(const ConvertPool&) = delete;

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

// Band `band` of `bands` over rows [0, h): [r0, r1) with r0 even and r1 even or h, so each
// 4:2:0 chroma row (made from rows 2k and 2k + 1) falls in one band. Bands are whole rows,
// so v210's 6-pixel / 16-byte groups, which run along a row, are never split either. The
// bands tile [0, h) in order.
void BandRows(int h, int bands, int band, int& r0, int& r1);
// How many bands an eye h rows tall gets on `threads`: one per thread, but at least
// kMinBandRows rows each (a smaller band is not worth waking a thread for).
constexpr int kMinBandRows = 64;
int BandsFor(int h, int threads);

// One eye to convert: `fmt` pixels w x h (rows srcRowBytes apart) into *dst at column x0,
// or, with no dst, into `planes` at column x0 with the matrix tag into *matrixOut (DeckLink
// direct pairing: an eye straight into its half of a ring frame).
struct EyeJob {
    RawFormat fmt = RawFormat::Uyvy;
    const uint8_t* src = nullptr;
    int srcRowBytes = 0, w = 0, h = 0;
    P016* dst = nullptr;
    P016Planes planes;                    // used when dst is null
    ColorMatrix* matrixOut = nullptr;     // ...and the tag goes here (may be null)
    int x0 = 0;
    ColorMatrix rgbMatrix = ColorMatrix::BT709;  // r210: the matrix to encode with
};

// Converts eyes[0..n) together over `pool` (nullptr: one pass each on this thread). The
// pool's threads are shared out between the eyes and each eye is split into row bands, so
// a dual-stream input's left and right bands run at the same time. Each eye ends as ToP016
// alone leaves it, bit for bit, and the tags are set in order (eye n-1's last, as calling
// ToP016 for each in turn). Eyes may share a dst (side by side) when their columns do not
// overlap.
void ToP016Banded(ConvertPool* pool, const EyeJob* eyes, int n);
void ToP016Banded(ConvertPool* pool, RawFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, P016& dst,
                  int x0, ColorMatrix rgbMatrix);

// The capture's last step, the converted frame into the ring's planes (16-bit samples; y is
// width x height, uv width x height/2), also in row bands over `pool` (nullptr: this thread):
// a 4K pair is ~50 MB to copy, ~10 ms on one thread, on the lane that the other eye's lane
// waits for. Bit-identical to the one-pass copy; nothing outside the frame is written.
// Two eyes side by side, each cropped to cw x ch (ch even, each eye at least that big):
// y and uv are 2*cw samples wide (DeckLink two-input stereo, LiveSourceBase::PublishPair).
void ComposePairBanded(ConvertPool* pool, const P016& l, const P016& r, int cw, int ch, uint16_t* y, uint16_t* uv);
// One eye to read for the compose: a P016's planes (EyeOf), or one half of a side-by-side
// frame (DeckLink direct pairing's fallback reads an eye where its lane converted it).
// Rows are `stride` samples apart; the eye is width x height.
struct EyeView {
    const uint16_t* y = nullptr;
    const uint16_t* uv = nullptr;
    int stride = 0, width = 0, height = 0;
};
inline EyeView EyeOf(const P016& f) { return {f.y.data(), f.uv.data(), f.width, f.width, f.height}; }
// The same compose from views: the P016 one is this over EyeOf(l), EyeOf(r).
void ComposePairBanded(ConvertPool* pool, const EyeView& l, const EyeView& r, int cw, int ch, uint16_t* y,
                       uint16_t* uv);
// One frame as it is: y gets src.y, uv gets src.uv (LiveSourceBase::Publish).
void CopyFrameBanded(ConvertPool* pool, const P016& src, uint16_t* y, uint16_t* uv);

// The conversion threads for one capture input (its own thread included). `env` is
// MEDIAPLAYER_DECKLINK_CONVERT_THREADS: a number >= 1 sets them (at most 64; 1 = no
// workers, every eye in one pass on the input's own thread, as before). Otherwise: per eye, half the logical
// processors shared between the `eyes` the whole capture converts at once, 1 to 8, times the
// `inputEyes` this input carries (2 for a dual-stream input). 64 logical processors and two
// inputs: 8 each; one dual-stream input: 16; 8 logical, two inputs: 2 each.
// hardwareThreads 0 (unknown) counts as 1.
int ConvertThreads(int eyes, int inputEyes, unsigned hardwareThreads, const char* env);
// The same from std::thread::hardware_concurrency() and the environment.
int ConvertThreads(int eyes, int inputEyes);

}  // namespace mp::livepix
