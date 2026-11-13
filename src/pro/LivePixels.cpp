// SPDX-License-Identifier: Apache-2.0
#include "pro/LivePixels.h"

#include <algorithm>
#include <cstring>

namespace mp::livepix {

namespace {

// The BGRA encode, one copy for the NV12 and P016 converters. Limited range, fixed point
// (x256). BT.709 for HD and up, BT.601 for SD.
struct BgraEncode {
    bool hd;
    int kr, kg, kb;  // luma weights /256
    float cbB, crR;
    explicit BgraEncode(int h)
        : hd(h >= 720), kr(hd ? 54 : 77), kg(hd ? 183 : 150), kb(hd ? 19 : 29), cbB(hd ? 1.8556f : 1.772f),
          crR(hd ? 1.5748f : 1.402f) {}
    ColorMatrix Matrix() const { return hd ? ColorMatrix::BT709 : ColorMatrix::BT601; }
    // One pixel's limited-range luma (16..235).
    int Luma(const uint8_t* p) const {
        const int yy = (kr * p[2] + kg * p[1] + kb * p[0]) >> 8;  // full-range luma
        return 16 + ((yy * 219 + 127) / 255);
    }
    // A 2x2 block's chroma (16..240): a = its two pixels on the upper row, b = the lower.
    void Chroma(const uint8_t* a, const uint8_t* b, int& cbOut, int& crOut) const {
        float r = 0, g = 0, bl = 0;
        const uint8_t* q[4] = {a, a + 4, b, b + 4};
        for (const uint8_t* p : q) {
            bl += p[0];
            g += p[1];
            r += p[2];
        }
        r *= 0.25f;
        g *= 0.25f;
        bl *= 0.25f;
        const float yy = (kr * r + kg * g + kb * bl) / 256.0f;
        const float cb = (bl - yy) / cbB, cr = (r - yy) / crR;  // -127.5 .. 127.5
        cbOut = std::clamp((int)(128.0f + cb * 224.0f / 255.0f + 0.5f), 16, 240);
        crOut = std::clamp((int)(128.0f + cr * 224.0f / 255.0f + 0.5f), 16, 240);
    }
};

// ---- P016 row-band kernels: rows [r0, r1) of one eye into dst at column x0 (ToP016Rows
// has checked the arguments: r0 even, r1 even or h). Luma rows r0..r1-1, chroma rows
// r0/2..r1/2-1; a band ending at an odd h also has the last row's luma.

void UyvyRows(const uint8_t* src, int rb, int w, const P016Planes& dst, int x0, int r0, int r1) {
    const int pairs = w / 2;
    for (int row = r0; row < r1; ++row) {
        const uint8_t* s = src + (size_t)row * rb;
        uint16_t* yo = dst.y + (size_t)row * dst.width + x0;
        for (int i = 0; i < pairs; ++i) {
            yo[2 * i] = (uint16_t)(s[4 * i + 1] << 8);
            yo[2 * i + 1] = (uint16_t)(s[4 * i + 3] << 8);
        }
        if (w & 1) yo[w - 1] = 0;  // an odd last column has no UYVY pair (0, as it always was)
    }
    for (int row = r0 / 2; row < r1 / 2; ++row) {
        const uint8_t* a = src + (size_t)(2 * row) * rb;
        const uint8_t* b = a + rb;
        uint16_t* uvo = dst.uv + (size_t)row * dst.width + x0;
        for (int i = 0; i < pairs; ++i) {
            uvo[2 * i] = (uint16_t)(((a[4 * i] + b[4 * i] + 1) >> 1) << 8);              // Cb
            uvo[2 * i + 1] = (uint16_t)(((a[4 * i + 2] + b[4 * i + 2] + 1) >> 1) << 8);  // Cr
        }
        if (w & 1) uvo[w - 1] = 0;
    }
}

void BgraRows(const uint8_t* src, int rb, int w, int h, const P016Planes& dst, int x0, int r0, int r1) {
    const BgraEncode enc(h);
    for (int row = r0; row < r1; ++row) {
        const uint8_t* s = src + (size_t)row * rb;
        uint16_t* yo = dst.y + (size_t)row * dst.width + x0;
        for (int x = 0; x < w; ++x) yo[x] = (uint16_t)(enc.Luma(s + 4 * x) << 8);
    }
    for (int row = r0 / 2; row < r1 / 2; ++row) {
        const uint8_t* a = src + (size_t)(2 * row) * rb;
        const uint8_t* b = a + rb;
        uint16_t* uvo = dst.uv + (size_t)row * dst.width + x0;
        for (int i = 0; i < w / 2; ++i) {
            int cb, cr;
            enc.Chroma(a + 8 * i, b + 8 * i, cb, cr);
            uvo[2 * i] = (uint16_t)(cb << 8);
            uvo[2 * i + 1] = (uint16_t)(cr << 8);
        }
        if (w & 1) uvo[w - 1] = 0;
    }
}

// w even.
void V210Rows(const uint8_t* src, int rb, int w, const P016Planes& dst, int x0, int r0, int r1) {
    // One pass per row pair, straight into dst: a 4K60 eye has ~16 ms, and the
    // unpack-to-scratch-then-copy version this replaced took 15.6 ms of it (#141).
    // Group = 6 pixels in 4 words: Cb0 Y0 Cr0 | Y1 Cb2 Y2 | Cr2 Y3 Cb4 | Y4 Cr4 Y5.
    // A band is whole rows, so it never splits a group (groups run along a row).
    auto s = [](uint32_t v, int k) { return (uint16_t)((v >> (10 * k)) & 0x3ff); };
    const int full = w / 6;  // whole groups; a width that is not a multiple of 6 ends in a partial one
    auto rowY = [&](const uint32_t* wd, uint16_t* yo) {
        for (int g = 0; g < full; ++g, wd += 4, yo += 6) {
            yo[0] = (uint16_t)(s(wd[0], 1) << 6);
            yo[1] = (uint16_t)(s(wd[1], 0) << 6);
            yo[2] = (uint16_t)(s(wd[1], 2) << 6);
            yo[3] = (uint16_t)(s(wd[2], 1) << 6);
            yo[4] = (uint16_t)(s(wd[3], 0) << 6);
            yo[5] = (uint16_t)(s(wd[3], 2) << 6);
        }
        if (w == 6 * full) return;  // (not past the last row's end when it fits the padding exactly)
        const uint16_t yy[6] = {s(wd[0], 1), s(wd[1], 0), s(wd[1], 2), s(wd[2], 1), s(wd[3], 0), s(wd[3], 2)};
        for (int k = 0; k < w - 6 * full; ++k) yo[k] = (uint16_t)(yy[k] << 6);  // the group is in the row's padding
    };
    auto avg = [](uint16_t a, uint16_t b) { return (uint16_t)(((a + b + 1) >> 1) << 6); };
    for (int row = r0; row + 1 < r1; row += 2) {
        const uint32_t* a = reinterpret_cast<const uint32_t*>(src + (size_t)row * rb);
        const uint32_t* b = reinterpret_cast<const uint32_t*>(src + (size_t)(row + 1) * rb);
        rowY(a, dst.y + (size_t)row * dst.width + x0);
        rowY(b, dst.y + (size_t)(row + 1) * dst.width + x0);
        // 4:2:2 -> 4:2:0: average the two rows' chroma
        uint16_t* uvo = dst.uv + (size_t)(row / 2) * dst.width + x0;
        for (int g = 0, pair = 0; pair < w / 2; ++g, pair += 3) {
            const uint32_t* p = a + 4 * g;
            const uint32_t* q = b + 4 * g;
            const uint16_t c[6] = {avg(s(p[0], 0), s(q[0], 0)), avg(s(p[0], 2), s(q[0], 2)),
                                   avg(s(p[1], 1), s(q[1], 1)), avg(s(p[2], 0), s(q[2], 0)),
                                   avg(s(p[2], 2), s(q[2], 2)), avg(s(p[3], 1), s(q[3], 1))};
            const int n = std::min(3, w / 2 - pair);
            std::memcpy(uvo + 2 * pair, c, sizeof(uint16_t) * 2 * (size_t)n);
        }
    }
    if (r1 & 1)  // the band ends at an odd h: the last row's luma (no chroma row to pair with)
        rowY(reinterpret_cast<const uint32_t*>(src + (size_t)(r1 - 1) * rb),
             dst.y + (size_t)(r1 - 1) * dst.width + x0);
}

// w even.
void R210Rows(const uint8_t* src, int rb, int w, const P016Planes& dst, int x0, int r0, int r1, ColorMatrix matrix) {
    const float kr = matrix == ColorMatrix::BT601 ? 0.299f : matrix == ColorMatrix::BT2020 ? 0.2627f : 0.2126f;
    const float kb = matrix == ColorMatrix::BT601 ? 0.114f : matrix == ColorMatrix::BT2020 ? 0.0593f : 0.0722f;
    const float kg = 1.0f - kr - kb;
    // Limited in, limited out: with the 10-bit code c, Y'16 = 64 * (kr R + kg G + kb B)
    // exactly (64 -> 16 << 8, 940 -> 235 << 8); chroma (224 / 219 of the luma swing):
    // C16 = 32768 + 57344 / (876 * 2 (1 - k)) * (component - Y'), in 10-bit codes.
    const float sCb = 57344.0f / (876.0f * 2.0f * (1.0f - kb)), sCr = 57344.0f / (876.0f * 2.0f * (1.0f - kr));
    auto px = [](const uint8_t* p, float& r, float& g, float& b) {
        const uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        r = (float)((v >> 20) & 0x3ff);
        g = (float)((v >> 10) & 0x3ff);
        b = (float)(v & 0x3ff);
    };
    auto clamp16 = [](float v) { return (uint16_t)std::clamp((int)(v + 0.5f), 0, 65535); };
    for (int row = r0; row + 1 < r1; row += 2) {
        const uint8_t* a = src + (size_t)row * rb;
        const uint8_t* b = a + rb;
        uint16_t* ya = dst.y + (size_t)row * dst.width + x0;
        uint16_t* yb = ya + dst.width;
        uint16_t* uvo = dst.uv + (size_t)(row / 2) * dst.width + x0;
        for (int x = 0; x < w; x += 2) {
            float r[4], g[4], bl[4];
            px(a + 4 * x, r[0], g[0], bl[0]);
            px(a + 4 * x + 4, r[1], g[1], bl[1]);
            px(b + 4 * x, r[2], g[2], bl[2]);
            px(b + 4 * x + 4, r[3], g[3], bl[3]);
            ya[x] = clamp16(64.0f * (kr * r[0] + kg * g[0] + kb * bl[0]));
            ya[x + 1] = clamp16(64.0f * (kr * r[1] + kg * g[1] + kb * bl[1]));
            yb[x] = clamp16(64.0f * (kr * r[2] + kg * g[2] + kb * bl[2]));
            yb[x + 1] = clamp16(64.0f * (kr * r[3] + kg * g[3] + kb * bl[3]));
            const float mr = 0.25f * (r[0] + r[1] + r[2] + r[3]), mg = 0.25f * (g[0] + g[1] + g[2] + g[3]),
                        mb = 0.25f * (bl[0] + bl[1] + bl[2] + bl[3]);
            const float my = kr * mr + kg * mg + kb * mb;
            uvo[x] = clamp16(32768.0f + sCb * (mb - my));
            uvo[x + 1] = clamp16(32768.0f + sCr * (mr - my));
        }
    }
    if (r1 & 1) {  // the band ends at an odd h: the last row's luma (no chroma row to pair with)
        const uint8_t* a = src + (size_t)(r1 - 1) * rb;
        uint16_t* yo = dst.y + (size_t)(r1 - 1) * dst.width + x0;
        for (int x = 0; x < w; ++x) {
            float r, g, bl;
            px(a + 4 * x, r, g, bl);
            yo[x] = clamp16(64.0f * (kr * r + kg * g + kb * bl));
        }
    }
}

}  // namespace

void Nv12::Resize(int w, int h) {
    width = w;
    height = h;
    const size_t ys = (size_t)w * (size_t)h, uvs = (size_t)w * (size_t)(h / 2);
    if (y.size() != ys) y.resize(ys);
    if (uv.size() != uvs) uv.resize(uvs);
}

void UyvyToNv12(const uint8_t* src, int srcRowBytes, int w, int h, Nv12& dst, int x0) {
    if (!src || w <= 0 || h <= 1 || x0 < 0 || x0 + w > dst.width || h > dst.height) return;
    dst.matrix = DefaultMatrixForHeight(h);  // YUV passes through: the signal's own matrix
    const int pairs = w / 2;
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + (size_t)row * srcRowBytes;
        uint8_t* yo = dst.y.data() + (size_t)row * dst.width + x0;
        for (int i = 0; i < pairs; ++i) {
            yo[2 * i] = s[4 * i + 1];
            yo[2 * i + 1] = s[4 * i + 3];
        }
    }
    for (int row = 0; row < h / 2; ++row) {
        const uint8_t* a = src + (size_t)(2 * row) * srcRowBytes;
        const uint8_t* b = a + srcRowBytes;
        uint8_t* uvo = dst.uv.data() + (size_t)row * dst.width + x0;  // x0 even: x0 bytes in
        for (int i = 0; i < pairs; ++i) {
            uvo[2 * i] = (uint8_t)((a[4 * i] + b[4 * i] + 1) >> 1);              // Cb
            uvo[2 * i + 1] = (uint8_t)((a[4 * i + 2] + b[4 * i + 2] + 1) >> 1);  // Cr
        }
    }
}

void BgraToNv12(const uint8_t* src, int srcRowBytes, int w, int h, Nv12& dst, int x0) {
    if (!src || w <= 0 || h <= 1 || x0 < 0 || x0 + w > dst.width || h > dst.height) return;
    const BgraEncode enc(h);
    dst.matrix = enc.Matrix();  // tag = the encode below
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + (size_t)row * srcRowBytes;
        uint8_t* yo = dst.y.data() + (size_t)row * dst.width + x0;
        for (int x = 0; x < w; ++x) yo[x] = (uint8_t)enc.Luma(s + 4 * x);
    }
    for (int row = 0; row < h / 2; ++row) {
        const uint8_t* a = src + (size_t)(2 * row) * srcRowBytes;
        const uint8_t* b = a + srcRowBytes;
        uint8_t* uvo = dst.uv.data() + (size_t)row * dst.width + x0;
        for (int i = 0; i < w / 2; ++i) {
            int cb, cr;
            enc.Chroma(a + 8 * i, b + 8 * i, cb, cr);
            uvo[2 * i] = (uint8_t)cb;
            uvo[2 * i + 1] = (uint8_t)cr;
        }
    }
}

void P016::Resize(int w, int h) {
    width = w;
    height = h;
    const size_t ys = (size_t)w * (size_t)h, uvs = (size_t)w * (size_t)(h / 2);
    if (y.size() != ys) y.resize(ys);
    if (uv.size() != uvs) uv.resize(uvs);
}

bool EyeFits(const uint8_t* src, int w, int h, const P016Planes& dst, int x0) {
    return src && w > 0 && h > 1 && x0 >= 0 && x0 + w <= dst.width && h <= dst.height;
}

bool EyeFits(const uint8_t* src, int w, int h, const P016& dst, int x0) {
    return src && w > 0 && h > 1 && x0 >= 0 && x0 + w <= dst.width && h <= dst.height;
}

ColorMatrix P016Matrix(RawFormat fmt, int h, ColorMatrix rgbMatrix) {
    switch (fmt) {
        case RawFormat::Bgra: return BgraEncode(h).Matrix();  // the encode's
        case RawFormat::R210: return rgbMatrix;               // the encode's
        case RawFormat::Uyvy:
        case RawFormat::V210: break;
    }
    return DefaultMatrixForHeight(h);  // YUV passes through: the SDI rule
}

bool ToP016Rows(RawFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, const P016Planes& dst, int x0,
                int r0, int r1, ColorMatrix rgbMatrix) {
    if (!EyeFits(src, w, h, dst, x0) || r0 < 0 || r0 > r1 || r1 > h || (r0 & 1) || ((r1 & 1) && r1 != h))
        return false;
    switch (fmt) {
        case RawFormat::Uyvy: UyvyRows(src, srcRowBytes, w, dst, x0, r0, r1); break;
        case RawFormat::V210: V210Rows(src, srcRowBytes, w & ~1, dst, x0, r0, r1); break;
        case RawFormat::Bgra: BgraRows(src, srcRowBytes, w, h, dst, x0, r0, r1); break;
        case RawFormat::R210: R210Rows(src, srcRowBytes, w & ~1, dst, x0, r0, r1, rgbMatrix); break;
    }
    return true;
}

bool ToP016Rows(RawFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0, int r0,
                int r1, ColorMatrix rgbMatrix) {
    return ToP016Rows(fmt, src, srcRowBytes, w, h, Planes(dst), x0, r0, r1, rgbMatrix);
}

void ToP016(RawFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0,
            ColorMatrix rgbMatrix) {
    if (!EyeFits(src, w, h, dst, x0)) return;
    dst.matrix = P016Matrix(fmt, h, rgbMatrix);
    ToP016Rows(fmt, src, srcRowBytes, w, h, dst, x0, 0, h, rgbMatrix);
}

void V210ToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0) {
    ToP016(RawFormat::V210, src, srcRowBytes, w, h, dst, x0, ColorMatrix::BT709);
}

void UyvyToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0) {
    ToP016(RawFormat::Uyvy, src, srcRowBytes, w, h, dst, x0, ColorMatrix::BT709);
}

void BgraToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0) {
    ToP016(RawFormat::Bgra, src, srcRowBytes, w, h, dst, x0, ColorMatrix::BT709);
}

void R210ToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0, ColorMatrix matrix) {
    ToP016(RawFormat::R210, src, srcRowBytes, w, h, dst, x0, matrix);
}

void CopyEye(P016& dst, int x0, int x1, int w) {
    if (w <= 0 || x0 < 0 || x1 < 0 || x0 + w > dst.width || x1 + w > dst.width) return;
    for (int row = 0; row < dst.height; ++row)
        std::memmove(dst.y.data() + (size_t)row * dst.width + x1, dst.y.data() + (size_t)row * dst.width + x0,
                     (size_t)w * 2);
    for (int row = 0; row < dst.height / 2; ++row)
        std::memmove(dst.uv.data() + (size_t)row * dst.width + x1, dst.uv.data() + (size_t)row * dst.width + x0,
                     (size_t)w * 2);
}

void CopyEye(Nv12& dst, int x0, int x1, int w) {
    if (w <= 0 || x0 < 0 || x1 < 0 || x0 + w > dst.width || x1 + w > dst.width) return;
    for (int row = 0; row < dst.height; ++row)
        std::memmove(dst.y.data() + (size_t)row * dst.width + x1,
                     dst.y.data() + (size_t)row * dst.width + x0, (size_t)w);
    for (int row = 0; row < dst.height / 2; ++row)
        std::memmove(dst.uv.data() + (size_t)row * dst.width + x1,
                     dst.uv.data() + (size_t)row * dst.width + x0, (size_t)w);
}

}  // namespace mp::livepix
