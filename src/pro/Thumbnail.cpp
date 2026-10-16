// SPDX-License-Identifier: Apache-2.0
#include "pro/Thumbnail.h"

#include <algorithm>
#include <cmath>

namespace mp::pro {

namespace {

constexpr int kTaps = 3;  // samples per axis per output pixel (box filter)

uint8_t Clamp8(float v) { return (uint8_t)std::clamp((int)std::lround(v), 0, 255); }

}  // namespace

bool MakeThumbnail(const FrameRing::Frame& f, StereoLayout layout, int outW, int outH, std::vector<uint8_t>& rgba) {
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

    rgba.assign((size_t)outW * outH * 4, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
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
                    r += yv + 1.5748f * cr;
                    g += yv - 0.1873f * cb - 0.4681f * cr;
                    b += yv + 1.8556f * cb;
                }
            }
            constexpr float inv = 1.0f / (kTaps * kTaps);
            uint8_t* o = &rgba[((size_t)(y0 + oy) * outW + (x0 + ox)) * 4];
            o[0] = Clamp8(r * inv);
            o[1] = Clamp8(g * inv);
            o[2] = Clamp8(b * inv);
        }
    }
    return true;
}

}  // namespace mp::pro
