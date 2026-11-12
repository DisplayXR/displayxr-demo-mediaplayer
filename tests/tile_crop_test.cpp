// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the media browser's TileCrop (#180): the square a grid tile shows (one
// eye, no black bars, unsqueezed), and the thumbnail size that fills a tile 1:1 without
// overflowing an atlas slot. Synthetic images, no fixtures.

#include "browser/TileCrop.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using mp::StereoLayout;
using mp::browser::ShownRegion;
using mp::browser::ThumbnailEdge;
using mp::browser::TileRegion;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

bool Is(const TileRegion& r, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

// A w x h textured picture, black outside [x0, x1) x [y0, y1) of each eye's `ew` x `eh` cell.
std::vector<uint8_t> Frame(uint32_t w, uint32_t h, uint32_t ew, uint32_t eh, uint32_t x0, uint32_t y0, uint32_t x1,
                           uint32_t y1) {
    std::vector<uint8_t> px((size_t)w * h * 4, 255);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t ex = x % ew, ey = y % eh;
            const bool in = ex >= x0 && ex < x1 && ey >= y0 && ey < y1;
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = p[1] = p[2] = in ? (uint8_t)(60 + (x * 7 + y * 13) % 150) : 0;
        }
    }
    return px;
}

void TestGeometry() {
    // A flat 16:9 picture: its centre square.
    CHECK(Is(ShownRegion(nullptr, 640, 360, StereoLayout::Mono), 140, 0, 360, 360), "mono: centre square");
    CHECK(Is(ShownRegion(nullptr, 360, 640, StereoLayout::Mono), 0, 140, 360, 360), "portrait: centre square");
    // Full side by side: the left eye's centre square.
    CHECK(Is(ShownRegion(nullptr, 1280, 360, StereoLayout::SbsFull), 140, 0, 360, 360), "full SBS: left eye");
    // Half side by side: the left eye is 320 x 360 px shown 640 x 360, so a square on screen
    // is 180 x 360 px.
    CHECK(Is(ShownRegion(nullptr, 640, 360, StereoLayout::SbsHalf), 70, 0, 180, 360), "half SBS: unsqueezed");
    // Top-bottom.
    CHECK(Is(ShownRegion(nullptr, 640, 720, StereoLayout::TbFull), 140, 0, 360, 360), "full TB: top eye");
    CHECK(Is(ShownRegion(nullptr, 640, 360, StereoLayout::TbHalf), 140, 0, 360, 180), "half TB: unsqueezed");
    // One view already (a LIF, a two-track video): the whole thumbnail.
    CHECK(Is(ShownRegion(nullptr, 640, 360, StereoLayout::Dual), 140, 0, 360, 360), "dual: whole thumbnail");
    CHECK(Is(ShownRegion(nullptr, 0, 0, StereoLayout::Mono), 0, 0, 0, 0), "nothing from nothing");
}

void TestBars() {
    // A 2.39:1 film letterboxed in 16:9 (bars of 29 in 225): the square is the film's height.
    std::vector<uint8_t> f = Frame(400, 225, 400, 225, 0, 29, 400, 196);
    CHECK(Is(ShownRegion(f.data(), 400, 225, StereoLayout::Mono), 116, 29, 167, 167), "letterbox: square of the film");

    // Full SBS, each eye pillarboxed (4:3 in 16:9): the left eye's 300 x 225 picture.
    f = Frame(800, 225, 400, 225, 50, 0, 350, 225);
    CHECK(Is(ShownRegion(f.data(), 800, 225, StereoLayout::SbsFull), 87, 0, 225, 225),
          "SBS: bars of the left eye, not of the whole frame");

    // A dark band at the top only is picture, not a bar.
    f = Frame(400, 225, 400, 225, 0, 60, 400, 225);
    CHECK(Is(ShownRegion(f.data(), 400, 225, StereoLayout::Mono), 87, 0, 225, 225), "a dark sky stays");
}

void TestThumbnailEdge() {
    const uint32_t need = 400, slot = 512, cap = 2048;
    // 16:9 flat: the short side must reach 400 -> 712 x 400 (the old fit-in-512 gave 288).
    CHECK(ThumbnailEdge(1920, 1080, StereoLayout::Mono, need, slot, cap) == 712, "mono 16:9");
    // 4:3 portrait photo: 400 x 534.
    CHECK(ThumbnailEdge(3000, 4000, StereoLayout::Mono, need, slot, cap) == 534, "portrait photo");
    // Full SBS 32:9: the eye's height is the square's, 1423 x 400.
    CHECK(ThumbnailEdge(3840, 1080, StereoLayout::SbsFull, need, slot, cap) == 1423, "full SBS");
    // Half SBS 16:9: the square is h/2 x h px, so the slot (512 tall) limits it: 910 x 512.
    CHECK(ThumbnailEdge(1920, 1080, StereoLayout::SbsHalf, need, slot, cap) == 911, "half SBS: slot-limited");
    // Never above the picture's own size.
    CHECK(ThumbnailEdge(640, 360, StereoLayout::Mono, need, slot, cap) == 640, "no upscale");
    // Never above the cap: a very wide panorama.
    CHECK(ThumbnailEdge(20000, 1000, StereoLayout::Mono, need, slot, cap) == 2048, "capped");
    // Unknown size: taken as 16:9.
    CHECK(ThumbnailEdge(0, 0, StereoLayout::Mono, need, slot, cap) == 712, "unknown size");
}

}  // namespace

int main() {
    TestGeometry();
    TestBars();
    TestThumbnailEdge();
    if (g_failures) {
        std::fprintf(stderr, "tile_crop_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("tile_crop_test: all passed\n");
    return 0;
}
