// SPDX-License-Identifier: Apache-2.0
#include "browser/TileCrop.h"

#include "media/Letterbox.h"

#include <algorithm>
#include <cmath>

namespace mp::browser {

namespace {

// The eye a tile shows, and how many screen pixels one of its pixels covers across (sx)
// and down (sy).
void Eye(uint32_t w, uint32_t h, StereoLayout layout, TileRegion& eye, uint32_t& sx, uint32_t& sy) {
    eye = TileRegion{0, 0, w, h};
    sx = sy = 1;
    switch (layout) {
        case StereoLayout::SbsHalf:
            sx = 2;
            [[fallthrough]];
        case StereoLayout::SbsFull:
            eye.w = w / 2;
            break;
        case StereoLayout::TbHalf:
            sy = 2;
            [[fallthrough]];
        case StereoLayout::TbFull:
            eye.h = h / 2;
            break;
        default:
            break;
    }
}

// The centre square on screen of `area`.
TileRegion Square(const TileRegion& area, uint32_t sx, uint32_t sy) {
    const uint32_t side = std::min(area.w * sx, area.h * sy);  // screen units
    TileRegion r;
    r.w = std::max(1u, side / sx);
    r.h = std::max(1u, side / sy);
    r.x = area.x + (area.w - r.w) / 2;
    r.y = area.y + (area.h - r.h) / 2;
    return r;
}

} // namespace

TileRegion ShownRegion(const uint8_t* rgba, uint32_t w, uint32_t h, StereoLayout layout) {
    if (w == 0 || h == 0) return TileRegion{};
    TileRegion eye;
    uint32_t sx = 1, sy = 1;
    Eye(w, h, layout, eye, sx, sy);
    if (eye.w == 0 || eye.h == 0) eye = TileRegion{0, 0, w, h};
    ActiveArea a;
    if (rgba && FindActiveArea(rgba, (int)eye.w, (int)eye.h, (ptrdiff_t)w * 4, a)) {
        eye = TileRegion{eye.x + (uint32_t)a.x, eye.y + (uint32_t)a.y, (uint32_t)a.w, (uint32_t)a.h};
    }
    return Square(eye, sx, sy);
}

uint32_t ThumbnailEdge(uint32_t w, uint32_t h, StereoLayout layout, uint32_t need, uint32_t slot, uint32_t cap) {
    if (w == 0 || h == 0) {
        w = 1600;
        h = 900;
    }
    const TileRegion r = ShownRegion(nullptr, w, h, layout);
    const double shorter = std::max(1u, std::min(r.w, r.h)), longer = std::max(1u, std::max(r.w, r.h));
    const double scale = std::min({(double)need / shorter, (double)slot / longer, 1.0});
    const double edge = std::ceil(scale * (double)std::max(w, h));
    return (uint32_t)std::max(1.0, std::min(edge, (double)cap));
}

} // namespace mp::browser
