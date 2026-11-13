// SPDX-License-Identifier: Apache-2.0
//
// v210 frames in the live ring (#238): a capture card's raw 10-bit 4:2:2 rows, published as
// they arrive and decoded on the GPU (VulkanRenderer, shaders/v210_to_p016.comp) instead of
// converted to P016 on the CPU. Phase 1 is opt-in: MEDIAPLAYER_GPU_V210=1.
//
// A FrameRing::Frame with format V210 (FrameRing.h):
//   plane[e]   eye e's v210 rows, RowBytes(eyeW) bytes each, height rows (e < eyes)
//   eyes       1 (mono) or 2 (left, right): side by side, one eye per plane, so each eye
//              keeps the card's own row layout and a lane can write (later: DMA) straight
//              into its plane
//   width      eyes * eyeW, the frame as shown (the SBS width), height the rows
//   matrix / transfer / fullRange   the signal's tags, as for any YUV frame
// eyeW and height are even (Shape() says whether a frame is one the renderer takes).
//
// Header-only and pure apart from the availability flag: tests/v210_test.cpp.
#pragma once

#include "media/FrameRing.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mp::v210 {

// One row of a w-pixel v210 image: w rounded up to 48 pixels, 128 bytes per 48.
inline size_t RowBytes(int w) { return w > 0 ? (size_t)((w + 47) / 48) * 128 : 0; }

// MEDIAPLAYER_GPU_V210: 1 / on / true / yes (any case) asks for the GPU decode; anything
// else, or unset, leaves it off (phase 1 default).
inline bool Requested(const char* env) {
    if (!env) return false;
    std::string s;
    for (const char* p = env; *p; ++p) s += (char)std::tolower((unsigned char)*p);
    return s == "1" || s == "on" || s == "true" || s == "yes";
}
inline bool Requested() { return Requested(std::getenv("MEDIAPLAYER_GPU_V210")); }

// Set by the renderer once its decode pass is built (Requested() and the device takes it).
// Producers read it when they open: publish raw v210 only when it is on, else convert on
// the CPU as before. (A v210 frame reaching a renderer without the pass is still shown:
// DecodeToP016 on the CPU, with one WARN.)
inline std::atomic<bool>& GpuDecodeFlag() {
    static std::atomic<bool> on{false};
    return on;
}
inline bool GpuDecodeActive() { return GpuDecodeFlag().load(); }
inline void SetGpuDecodeActive(bool on) { GpuDecodeFlag().store(on); }

// A frame the renderer takes: V210, 1 or 2 eyes, eyeW and height even and > 0, planes big
// enough. eyes / eyeW out.
inline bool Shape(const FrameRing::Frame& f, int& eyes, int& eyeW) {
    eyes = f.eyes;
    eyeW = eyes > 0 ? f.width / eyes : 0;
    if (f.format != PixFormat::V210 || f.gpu || (eyes != 1 && eyes != 2) || f.height <= 0 || (f.height & 1) ||
        eyeW <= 0 || (eyeW & 1) || eyeW * eyes != f.width)
        return false;
    const size_t bytes = RowBytes(eyeW) * (size_t)f.height;
    for (int e = 0; e < eyes; ++e) {
        const size_t have = f.HasExt() ? (f.ext[e].data ? f.ext[e].bytes : 0) : f.plane[e].size();
        if (have < bytes) return false;
    }
    return true;
}

// Eye e's rows, wherever they are: the producer's host block (zero-copy) or plane[e].
inline const uint8_t* EyeRows(const FrameRing::Frame& f, int e) {
    return f.HasExt() ? f.ext[e].data : f.plane[e].data();
}

// MEDIAPLAYER_ZERO_COPY_UPLOAD (#238 phase 3): producers publish v210 rows in host blocks the GPU
// reads in place. Set by the renderer once it can import host memory (and the GPU decode is up).
inline std::atomic<bool>& ZeroCopyFlag() {
    static std::atomic<bool> on{false};
    return on;
}
inline bool ZeroCopyActive() { return ZeroCopyFlag().load(); }
inline void SetZeroCopyActive(bool on) { ZeroCopyFlag().store(on); }

// Pack one row: y(x) for x < w, cb(i) / cr(i) for the chroma pair i < w / 2 (pixels 2i,
// 2i + 1), all 10-bit. `row` gets RowBytes(w) bytes; the padding (and a last group's missing
// pixels) is zero. w even.
template <class Y, class Cb, class Cr>
void PackRow(uint8_t* row, int w, const Y& y, const Cb& cb, const Cr& cr) {
    std::memset(row, 0, RowBytes(w));
    uint32_t* o = reinterpret_cast<uint32_t*>(row);
    const auto at = [](const auto& f, int i, int n) -> uint32_t { return i < n ? (uint32_t)f(i) & 0x3FFu : 0u; };
    const int pairs = w / 2;
    for (int g = 0; 6 * g < w; ++g, o += 4) {
        const int x = 6 * g, c = 3 * g;
        o[0] = at(cb, c, pairs) | (at(y, x, w) << 10) | (at(cr, c, pairs) << 20);
        o[1] = at(y, x + 1, w) | (at(cb, c + 1, pairs) << 10) | (at(y, x + 2, w) << 20);
        o[2] = at(cr, c + 1, pairs) | (at(y, x + 3, w) << 10) | (at(cb, c + 2, pairs) << 20);
        o[3] = at(y, x + 4, w) | (at(cr, c + 2, pairs) << 10) | (at(y, x + 5, w) << 20);
    }
}

// The shader's arithmetic, compiled as C++: shaders/v210_decode.inc's functions become this
// struct's members, reading `src` and writing `dst` through the include's two macros.
struct Decoder {
    using uint = uint32_t;
    const uint32_t* src[2] = {nullptr, nullptr};
    uint32_t* dst = nullptr;
#define V210_LOAD(eye, i) (src[(eye)][(i)])
#define V210_STORE(i, v) (dst[(i)] = (v))
#include "../../shaders/v210_decode.inc"
#undef V210_LOAD
#undef V210_STORE
};

// The P016 output's layout, as the GPU pass writes it into one buffer: the Y plane (outW x h
// 16-bit samples), then at uvBase words the CbCr plane (outW / 2 x h / 2 pairs).
struct OutLayout {
    uint32_t outW = 0, h = 0;
    size_t yWords = 0, uvBase = 0, uvWords = 0, totalBytes = 0;
};
inline OutLayout OutLayoutFor(int eyes, int eyeW, int h) {
    OutLayout L;
    L.outW = (uint32_t)(eyes * eyeW);
    L.h = (uint32_t)h;
    L.yWords = (size_t)L.outW / 2 * L.h;
    L.uvBase = (L.yWords + 63) / 64 * 64;  // 256-byte aligned: a buffer-to-image copy offset
    L.uvWords = (size_t)L.outW / 2 * (L.h / 2);
    L.totalBytes = (L.uvBase + L.uvWords) * 4;
    return L;
}

// Every invocation of the GPU pass, on the CPU: eye e's rows at eye[e] (RowBytes(eyeW)
// apart), into `out` (OutLayoutFor's totalBytes, 4-byte aligned). The test's reference, and
// the renderer's fallback when it has no decode pass.
inline void DecodeToP016(const uint8_t* const eye[2], int eyes, int eyeW, int h, uint32_t* out) {
    const OutLayout L = OutLayoutFor(eyes, eyeW, h);
    Decoder d;
    d.src[0] = reinterpret_cast<const uint32_t*>(eye[0]);
    d.src[1] = reinterpret_cast<const uint32_t*>(eyes > 1 ? eye[1] : eye[0]);
    d.dst = out;
    const uint32_t pitchWords = (uint32_t)(RowBytes(eyeW) / 4), groups = (uint32_t)((eyeW + 5) / 6);
    for (uint32_t e = 0; e < (uint32_t)eyes; ++e)
        for (uint32_t r = 0; r < (uint32_t)h / 2; ++r)
            for (uint32_t g = 0; g < groups; ++g)
                d.v210DecodeGroup(e, g, r, (uint32_t)eyeW, pitchWords, e * (uint32_t)eyeW, L.outW,
                                  (uint32_t)L.uvBase);
}

}  // namespace mp::v210
