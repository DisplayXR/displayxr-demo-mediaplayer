// SPDX-License-Identifier: Apache-2.0
#include "pro/LivePixels.h"

#include <algorithm>
#include <cstring>

namespace mp::livepix {

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
    // Limited range, fixed point (x256). BT.709 for HD and up, BT.601 for SD.
    const bool hd = h >= 720;
    dst.matrix = hd ? ColorMatrix::BT709 : ColorMatrix::BT601;  // tag = the encode below
    const int kr = hd ? 54 : 77, kg = hd ? 183 : 150, kb = hd ? 19 : 29;  // luma weights /256
    auto lumaOf = [&](const uint8_t* p) {
        const int yy = (kr * p[2] + kg * p[1] + kb * p[0]) >> 8;  // full-range luma
        return 16 + ((yy * 219 + 127) / 255);
    };
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + (size_t)row * srcRowBytes;
        uint8_t* yo = dst.y.data() + (size_t)row * dst.width + x0;
        for (int x = 0; x < w; ++x) yo[x] = (uint8_t)lumaOf(s + 4 * x);
    }
    const float cbB = hd ? 1.8556f : 1.772f, crR = hd ? 1.5748f : 1.402f;
    for (int row = 0; row < h / 2; ++row) {
        const uint8_t* a = src + (size_t)(2 * row) * srcRowBytes;
        const uint8_t* b = a + srcRowBytes;
        uint8_t* uvo = dst.uv.data() + (size_t)row * dst.width + x0;
        for (int i = 0; i < w / 2; ++i) {
            float r = 0, g = 0, bl = 0;
            const uint8_t* q[4] = {a + 8 * i, a + 8 * i + 4, b + 8 * i, b + 8 * i + 4};
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
            uvo[2 * i] = (uint8_t)std::clamp((int)(128.0f + cb * 224.0f / 255.0f + 0.5f), 16, 240);
            uvo[2 * i + 1] = (uint8_t)std::clamp((int)(128.0f + cr * 224.0f / 255.0f + 0.5f), 16, 240);
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

void V210ToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0) {
    if (!src || w <= 0 || h <= 1 || x0 < 0 || x0 + w > dst.width || h > dst.height) return;
    dst.matrix = DefaultMatrixForHeight(h);
    w &= ~1;
    // one row -> Y (w) + chroma pairs (w/2 each), 10-bit values
    // per-thread scratch (two card threads may convert at once), sized once per format
    thread_local std::vector<uint16_t> ya, cba, cra, cbb, crb;
    ya.resize((size_t)w);
    for (std::vector<uint16_t>* v : {&cba, &cra, &cbb, &crb}) v->resize((size_t)w / 2 + 3);
    auto unpack = [&](const uint8_t* row, std::vector<uint16_t>& cb, std::vector<uint16_t>& cr, bool wantY) {
        const uint32_t* wd = reinterpret_cast<const uint32_t*>(row);
        for (int g = 0, px = 0; px < w; ++g, px += 6) {
            const uint32_t a = wd[4 * g], b = wd[4 * g + 1], c = wd[4 * g + 2], d = wd[4 * g + 3];
            const uint16_t s[12] = {(uint16_t)(a & 0x3ff), (uint16_t)((a >> 10) & 0x3ff), (uint16_t)((a >> 20) & 0x3ff),
                                    (uint16_t)(b & 0x3ff), (uint16_t)((b >> 10) & 0x3ff), (uint16_t)((b >> 20) & 0x3ff),
                                    (uint16_t)(c & 0x3ff), (uint16_t)((c >> 10) & 0x3ff), (uint16_t)((c >> 20) & 0x3ff),
                                    (uint16_t)(d & 0x3ff), (uint16_t)((d >> 10) & 0x3ff), (uint16_t)((d >> 20) & 0x3ff)};
            // Cb0 Y0 Cr0 Y1 Cb2 Y2 Cr2 Y3 Cb4 Y4 Cr4 Y5
            const int pair = px / 2;
            cb[(size_t)pair] = s[0];
            cr[(size_t)pair] = s[2];
            cb[(size_t)pair + 1] = s[4];
            cr[(size_t)pair + 1] = s[6];
            cb[(size_t)pair + 2] = s[8];
            cr[(size_t)pair + 2] = s[10];
            if (wantY) {
                const uint16_t yy[6] = {s[1], s[3], s[5], s[7], s[9], s[11]};
                for (int k = 0; k < 6 && px + k < w; ++k) ya[(size_t)(px + k)] = yy[k];
            }
        }
    };
    for (int row = 0; row < h; ++row) {
        const bool even = (row & 1) == 0;
        unpack(src + (size_t)row * srcRowBytes, even ? cba : cbb, even ? cra : crb, true);
        uint16_t* yo = dst.y.data() + (size_t)row * dst.width + x0;
        for (int x = 0; x < w; ++x) yo[x] = (uint16_t)(ya[(size_t)x] << 6);
        if (!even) {
            uint16_t* uvo = dst.uv.data() + (size_t)(row / 2) * dst.width + x0;
            for (int i = 0; i < w / 2; ++i) {
                uvo[2 * i] = (uint16_t)(((cba[(size_t)i] + cbb[(size_t)i] + 1) >> 1) << 6);
                uvo[2 * i + 1] = (uint16_t)(((cra[(size_t)i] + crb[(size_t)i] + 1) >> 1) << 6);
            }
        }
    }
}

void UyvyToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0) {
    if (!src || w <= 0 || h <= 1 || x0 < 0 || x0 + w > dst.width || h > dst.height) return;
    Nv12 tmp;
    tmp.Resize(w, h);
    UyvyToNv12(src, srcRowBytes, w, h, tmp, 0);
    dst.matrix = tmp.matrix;
    for (int row = 0; row < h; ++row)
        for (int x = 0; x < w; ++x)
            dst.y[(size_t)row * dst.width + x0 + x] = (uint16_t)(tmp.y[(size_t)row * w + x] << 8);
    for (int row = 0; row < h / 2; ++row)
        for (int x = 0; x < w; ++x)
            dst.uv[(size_t)row * dst.width + x0 + x] = (uint16_t)(tmp.uv[(size_t)row * w + x] << 8);
}

void BgraToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0) {
    if (!src || w <= 0 || h <= 1 || x0 < 0 || x0 + w > dst.width || h > dst.height) return;
    Nv12 tmp;
    tmp.Resize(w, h);
    BgraToNv12(src, srcRowBytes, w, h, tmp, 0);
    dst.matrix = tmp.matrix;
    for (int row = 0; row < h; ++row)
        for (int x = 0; x < w; ++x)
            dst.y[(size_t)row * dst.width + x0 + x] = (uint16_t)(tmp.y[(size_t)row * w + x] << 8);
    for (int row = 0; row < h / 2; ++row)
        for (int x = 0; x < w; ++x)
            dst.uv[(size_t)row * dst.width + x0 + x] = (uint16_t)(tmp.uv[(size_t)row * w + x] << 8);
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
