// SPDX-License-Identifier: Apache-2.0
//
// pro::MakeThumbnail: the left eye only, the right colours from each pixel format
// (NV12 / P016 / I420 limited + full range, RGBA), the eye's display aspect with black
// bars, and no thumbnail for GPU-only frames.

#include "pro/Thumbnail.h"

#include <cstdio>
#include <cstdlib>

using namespace mp;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

struct Yuv { int y, u, v; };
// BT.709 limited range: pure red / blue, and white.
constexpr Yuv kRed{63, 102, 240}, kBlue{32, 240, 118}, kWhite{235, 128, 128};

// An SBS frame: left eye `l`, right eye `r`.
FrameRing::Frame Nv12(int w, int h, Yuv l, Yuv r, bool p16 = false) {
    FrameRing::Frame f;
    f.width = w;
    f.height = h;
    f.format = p16 ? PixFormat::P016 : PixFormat::NV12;
    const int b = p16 ? 2 : 1;
    f.plane[0].resize((size_t)w * h * b);
    f.plane[1].resize((size_t)(w / 2) * (h / 2) * 2 * b);
    auto put = [&](std::vector<uint8_t>& p, size_t i, int v) {
        if (p16) {
            p[i * 2] = 0;
            p[i * 2 + 1] = (uint8_t)v;
        } else {
            p[i] = (uint8_t)v;
        }
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) put(f.plane[0], (size_t)y * w + x, x < w / 2 ? l.y : r.y);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            const Yuv& c = x < w / 4 ? l : r;
            const size_t i = ((size_t)y * (w / 2) + x) * 2;
            put(f.plane[1], i, c.u);
            put(f.plane[1], i + 1, c.v);
        }
    return f;
}

bool Near(const uint8_t* p, int r, int g, int b, int tol = 6) {
    return std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol && p[3] == 255;
}

const uint8_t* Px(const std::vector<uint8_t>& t, int w, int x, int y) { return &t[((size_t)y * w + x) * 4]; }

}  // namespace

int main() {
    std::vector<uint8_t> t;
    // Full SBS 3840x1080: each eye 1920x1080 = 16:9, fills a 160x90 thumb; left eye only.
    CHECK(pro::MakeThumbnail(Nv12(3840, 1080, kRed, kBlue), StereoLayout::SbsFull, 160, 90, t), "NV12 SBS");
    CHECK(t.size() == 160 * 90 * 4, "size");
    CHECK(Near(Px(t, 160, 5, 5), 255, 0, 0) && Near(Px(t, 160, 155, 85), 255, 0, 0), "left eye only (red, no blue)");

    CHECK(pro::MakeThumbnail(Nv12(3840, 1080, kWhite, kBlue, true), StereoLayout::SbsFull, 160, 90, t), "P016");
    CHECK(Near(Px(t, 160, 80, 45), 255, 255, 255), "P016 limited-range white -> 255");

    // Half SBS 1920x1080: each eye is 960 wide but shown at 16:9 -> fills the thumb.
    CHECK(pro::MakeThumbnail(Nv12(1920, 1080, kRed, kBlue), StereoLayout::SbsHalf, 160, 90, t), "half SBS");
    CHECK(Near(Px(t, 160, 2, 45), 255, 0, 0), "half SBS eye is unsqueezed (no side bars)");
    // The same frame as FULL SBS: the eye is 960x1080 (8:9) -> pillarboxed.
    CHECK(pro::MakeThumbnail(Nv12(1920, 1080, kRed, kBlue), StereoLayout::SbsFull, 160, 90, t), "full SBS 8:9");
    CHECK(Near(Px(t, 160, 2, 45), 0, 0, 0) && Near(Px(t, 160, 80, 45), 255, 0, 0), "8:9 eye pillarboxed");

    // Mono: the whole frame (left half red, right half blue).
    CHECK(pro::MakeThumbnail(Nv12(1920, 1080, kRed, kBlue), StereoLayout::Mono, 160, 90, t), "mono");
    CHECK(Near(Px(t, 160, 20, 45), 255, 0, 0) && Near(Px(t, 160, 140, 45), 0, 0, 255), "mono shows both halves");

    // Full range and I420.
    FrameRing::Frame i420;
    i420.width = 64;
    i420.height = 36;
    i420.format = PixFormat::I420;
    i420.fullRange = true;
    i420.plane[0].assign(64 * 36, 255);
    i420.plane[1].assign(32 * 18, 128);
    i420.plane[2].assign(32 * 18, 128);
    CHECK(pro::MakeThumbnail(i420, StereoLayout::Mono, 32, 18, t) && Near(Px(t, 32, 16, 9), 255, 255, 255), "I420 full-range white");

    FrameRing::Frame rgba;
    rgba.width = 8;
    rgba.height = 4;
    rgba.format = PixFormat::RGBA;
    rgba.plane[0].resize(8 * 4 * 4);
    for (int i = 0; i < 32; ++i) {
        rgba.plane[0][i * 4] = 10;
        rgba.plane[0][i * 4 + 1] = 200;
        rgba.plane[0][i * 4 + 2] = 30;
        rgba.plane[0][i * 4 + 3] = 255;
    }
    CHECK(pro::MakeThumbnail(rgba, StereoLayout::Mono, 8, 4, t) && Near(Px(t, 8, 4, 2), 10, 200, 30, 1), "RGBA");

    FrameRing::Frame gpu = Nv12(64, 36, kRed, kBlue);
    gpu.gpu = true;
    CHECK(!pro::MakeThumbnail(gpu, StereoLayout::SbsFull, 32, 18, t), "a GPU-only frame has no thumbnail");
    FrameRing::Frame shortBuf = Nv12(64, 36, kRed, kBlue);
    shortBuf.plane[1].resize(10);
    CHECK(!pro::MakeThumbnail(shortBuf, StereoLayout::SbsFull, 32, 18, t), "a short buffer is refused, not read past");

    if (g_failures) {
        std::fprintf(stderr, "thumbnail_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("thumbnail_test: all passed\n");
    return 0;
}
