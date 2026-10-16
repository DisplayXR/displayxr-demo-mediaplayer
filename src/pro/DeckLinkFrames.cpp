// SPDX-License-Identifier: Apache-2.0
#include "pro/DeckLinkFrames.h"

#include <cstdlib>
#include <cstring>

namespace mp::pro::dl {

int V210RowBytes(int width) { return ((width + 47) / 48) * 128; }

namespace {

inline uint32_t Word(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint8_t Top8(uint32_t w, int shift) { return (uint8_t)(((w >> shift) & 0x3ffu) >> 2); }

// Unpack one row to 8-bit Y (w bytes) and 4:2:2 chroma pairs (w/2 Cb, w/2 Cr).
void UnpackRow(SrcFormat fmt, const uint8_t* row, int w, uint8_t* y, uint8_t* cb, uint8_t* cr) {
    if (fmt == SrcFormat::UYVY8) {
        for (int x = 0; x < w / 2; ++x) {
            const uint8_t* p = row + x * 4;
            cb[x] = p[0];
            y[2 * x] = p[1];
            cr[x] = p[2];
            y[2 * x + 1] = p[3];
        }
        return;
    }
    // v210: each 16-byte group carries 6 pixels:
    //   w0 = Cb0 Y0 Cr0 | w1 = Y1 Cb2 Y2 | w2 = Cr2 Y3 Cb4 | w3 = Y4 Cr4 Y5
    // (components at bits 0-9, 10-19, 20-29).
    int px = 0, c = 0;
    for (int g = 0; px < w; ++g) {
        const uint8_t* p = row + g * 16;
        const uint32_t w0 = Word(p), w1 = Word(p + 4), w2 = Word(p + 8), w3 = Word(p + 12);
        const uint8_t ys[6] = {Top8(w0, 10), Top8(w1, 0), Top8(w1, 20), Top8(w2, 10), Top8(w3, 0), Top8(w3, 20)};
        const uint8_t cbs[3] = {Top8(w0, 0), Top8(w1, 10), Top8(w2, 20)};
        const uint8_t crs[3] = {Top8(w0, 20), Top8(w2, 0), Top8(w3, 10)};
        for (int i = 0; i < 6 && px < w; ++i) y[px++] = ys[i];
        for (int i = 0; i < 3 && c < w / 2; ++i, ++c) {
            cb[c] = cbs[i];
            cr[c] = crs[i];
        }
    }
}

}  // namespace

void ToNV12(SrcFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, uint8_t* dstY,
            uint8_t* dstUV, int dstStride, int dstX) {
    // Scratch for two rows of unpacked samples.
    const int half = w / 2;
    uint8_t* buf = (uint8_t*)std::malloc((size_t)(w + 2 * half) * 2);
    if (!buf) return;
    uint8_t* y0 = buf;
    uint8_t* cb0 = y0 + w;
    uint8_t* cr0 = cb0 + half;
    uint8_t* y1 = cr0 + half;
    uint8_t* cb1 = y1 + w;
    uint8_t* cr1 = cb1 + half;

    for (int row = 0; row < h; row += 2) {
        const bool haveSecond = row + 1 < h;
        UnpackRow(fmt, src + (size_t)row * srcRowBytes, w, y0, cb0, cr0);
        if (haveSecond) UnpackRow(fmt, src + (size_t)(row + 1) * srcRowBytes, w, y1, cb1, cr1);
        std::memcpy(dstY + (size_t)row * dstStride + dstX, y0, (size_t)w);
        if (haveSecond) std::memcpy(dstY + (size_t)(row + 1) * dstStride + dstX, y1, (size_t)w);
        uint8_t* uv = dstUV + (size_t)(row / 2) * dstStride + dstX;  // dstX even: UV pairs align
        for (int x = 0; x < half; ++x) {
            uv[2 * x] = haveSecond ? (uint8_t)((cb0[x] + cb1[x] + 1) >> 1) : cb0[x];
            uv[2 * x + 1] = haveSecond ? (uint8_t)((cr0[x] + cr1[x] + 1) >> 1) : cr0[x];
        }
    }
    std::free(buf);
}

void StereoPairer::Reset() {
    pending_[0] = pending_[1] = false;
    stats_ = Stats{};
}

bool StereoPairer::Offer(int eye, int64_t timestamp) {
    if (eye != 0 && eye != 1) return false;
    const int other = 1 - eye;
    if (pending_[eye]) ++stats_.unpaired;  // this eye's previous frame never found a partner
    pending_[eye] = true;
    ts_[eye] = timestamp;
    if (!pending_[other]) return false;
    const int64_t skew = ts_[1] - ts_[0];
    if (std::llabs(skew) <= tol_) {
        pending_[0] = pending_[1] = false;
        ++stats_.pairs;
        stats_.lastSkew = skew;
        if (std::llabs(skew) > stats_.maxAbsSkew) stats_.maxAbsSkew = std::llabs(skew);
        return true;
    }
    // Too far apart: the older frame can no longer be matched; drop it, keep the newer.
    const int older = ts_[0] < ts_[1] ? 0 : 1;
    pending_[older] = false;
    ++stats_.unpaired;
    return false;
}

}  // namespace mp::pro::dl
