// SPDX-License-Identifier: Apache-2.0
#include "pro/Thumbnail.h"

#include <algorithm>
#include <cmath>

namespace mp::pro {

namespace {

constexpr int kTaps = 3;  // samples per axis per output pixel (box filter)

uint8_t Clamp8(float v) { return (uint8_t)std::clamp((int)std::lround(v), 0, 255); }
uint16_t Clamp16(float v) { return (uint16_t)std::clamp((int)std::lround(v * 257.0f), 0, 65535); }

// R'G'B' from range-expanded Y'CbCr (0..255 scale) with the frame's matrix, the same
// coefficients as shaders/sbs.frag YuvToRgb (#169: this was BT.709 for every frame).
struct Coeffs {
    float rv, gu, gv, bu;
};
Coeffs CoeffsFor(ColorMatrix m) {
    if (m == ColorMatrix::BT601) return {1.402f, 0.344136f, 0.714136f, 1.772f};
    if (m == ColorMatrix::BT2020) return {1.4746f, 0.164553f, 0.571353f, 1.8814f};
    return {1.5748f, 0.1873f, 0.4681f, 1.8556f};
}

// The box filter, writing each output pixel through `put(index, r, g, b)` (0..255 scale,
// unrounded), so the 8-bit thumbnail and the 16-bit scope image share one path.
template <class T, class Put>
bool ThumbnailCore(const FrameRing::Frame& f, StereoLayout layout, int outW, int outH, std::vector<T>& rgba, T opaque,
                   Put put) {
    if (f.gpu || f.width < 2 || f.height < 2 || outW < 1 || outH < 1 || f.plane[0].empty()) return false;
    const int w = f.width, h = f.height;
    const bool sbs = layout == StereoLayout::SbsFull || layout == StereoLayout::SbsHalf;
    const int eyeW = sbs ? w / 2 : w;
    // Display aspect of one eye: a half-SBS eye is squeezed and shown at the full width.
    const double aspect = (layout == StereoLayout::SbsHalf ? (double)w : (double)eyeW) / (double)h;
    int fitW = outW, fitH = (int)std::lround(outW / aspect);
    if (fitH > outH) {
        fitH = outH;
        fitW = std::max(1, (int)std::lround(outH * aspect));
    }
    const int x0 = (outW - fitW) / 2, y0 = (outH - fitH) / 2;

    const bool p16 = f.format == PixFormat::P016;
    const size_t bpsY = p16 ? 2 : 1;
    size_t needY = (size_t)w * h * (f.format == PixFormat::RGBA ? 4 : bpsY);
    if (f.plane[0].size() < needY) return false;
    const size_t cw = (size_t)(w / 2), ch = (size_t)(h / 2);
    if (f.format == PixFormat::NV12 && f.plane[1].size() < cw * ch * 2) return false;
    if (p16 && f.plane[1].size() < cw * ch * 4) return false;
    if (f.format == PixFormat::I420 && (f.plane[1].size() < cw * ch || f.plane[2].size() < cw * ch)) return false;

    const uint8_t* Y = f.plane[0].data();
    const uint8_t* UV = f.plane[1].data();
    const uint8_t* V = f.plane[2].data();
    auto luma = [&](int x, int y) -> float {
        const size_t i = (size_t)y * w + x;
        return p16 ? (float)(Y[i * 2 + 1]) + Y[i * 2] / 256.0f : (float)Y[i];  // P016: little-endian, top byte
    };
    auto chroma = [&](int x, int y, float& cb, float& cr) {
        const size_t i = (size_t)(y / 2) * cw + (size_t)(x / 2);
        switch (f.format) {
        case PixFormat::NV12: cb = UV[i * 2]; cr = UV[i * 2 + 1]; break;
        case PixFormat::P016: cb = UV[i * 4 + 1] + UV[i * 4] / 256.0f; cr = UV[i * 4 + 3] + UV[i * 4 + 2] / 256.0f; break;
        default: cb = UV[i]; cr = V[i]; break;  // I420
        }
    };

    const Coeffs k = CoeffsFor(f.matrix);
    rgba.assign((size_t)outW * outH * 4, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = opaque;
    const double stepX = (double)eyeW / fitW, stepY = (double)h / fitH;
    for (int oy = 0; oy < fitH; ++oy) {
        for (int ox = 0; ox < fitW; ++ox) {
            float r = 0, g = 0, b = 0;
            for (int ty = 0; ty < kTaps; ++ty) {
                const int sy = std::min(h - 1, (int)((oy + (ty + 0.5) / kTaps) * stepY));
                for (int tx = 0; tx < kTaps; ++tx) {
                    const int sx = std::min(eyeW - 1, (int)((ox + (tx + 0.5) / kTaps) * stepX));
                    if (f.format == PixFormat::RGBA) {
                        const uint8_t* p = Y + ((size_t)sy * w + sx) * 4;
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        continue;
                    }
                    float yv = luma(sx, sy), cb = 128, cr = 128;
                    chroma(sx, sy, cb, cr);
                    if (f.fullRange) {
                        cb -= 128.0f;
                        cr -= 128.0f;
                    } else {
                        yv = (yv - 16.0f) * (255.0f / 219.0f);
                        cb = (cb - 128.0f) * (255.0f / 224.0f);
                        cr = (cr - 128.0f) * (255.0f / 224.0f);
                    }
                    r += yv + k.rv * cr;
                    g += yv - k.gu * cb - k.gv * cr;
                    b += yv + k.bu * cb;
                }
            }
            constexpr float inv = 1.0f / (kTaps * kTaps);
            put(((size_t)(y0 + oy) * outW + (x0 + ox)) * 4, r * inv, g * inv, b * inv);
        }
    }
    return true;
}

}  // namespace

bool MakeThumbnail(const FrameRing::Frame& f, StereoLayout layout, int outW, int outH, std::vector<uint8_t>& rgba) {
    return ThumbnailCore(f, layout, outW, outH, rgba, (uint8_t)255, [&](size_t i, float r, float g, float b) {
        rgba[i] = Clamp8(r);
        rgba[i + 1] = Clamp8(g);
        rgba[i + 2] = Clamp8(b);
    });
}

bool MakeThumbnail16(const FrameRing::Frame& f, StereoLayout layout, int outW, int outH, std::vector<uint16_t>& rgba) {
    return ThumbnailCore(f, layout, outW, outH, rgba, (uint16_t)65535, [&](size_t i, float r, float g, float b) {
        rgba[i] = Clamp16(r);
        rgba[i + 1] = Clamp16(g);
        rgba[i + 2] = Clamp16(b);
    });
}

}  // namespace mp::pro
