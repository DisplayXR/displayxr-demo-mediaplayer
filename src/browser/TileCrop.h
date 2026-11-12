// SPDX-License-Identifier: Apache-2.0
//
// TileCrop: the part of a thumbnail a square grid tile shows, and how big a thumbnail to
// ask for so that part fills the tile at about one thumbnail pixel per screen pixel.
// Pure; tests/tile_crop_test.cpp.
//
// A tile shows one eye: the left one of a side-by-side frame, the top one of a top-bottom
// frame, the whole picture otherwise (a LIF's or a two-track video's thumbnail is one
// view already). The eye loses its black bars (media/Letterbox.h), then the centre square
// on screen is kept. A half-packed eye is shown unsqueezed, so its square on screen is
// h/2 x h pixels side by side and w x w/2 top-bottom.
#pragma once

#include "media/StereoTypes.h"

#include <cstdint>

namespace mp::browser {

struct TileRegion {
    uint32_t x = 0, y = 0, w = 0, h = 0;  // px of the thumbnail
};

// The region of the w x h RGBA8 thumbnail at `rgba` a tile shows. `rgba` may be null:
// then no bars are looked for. A zero-size region for a zero-size thumbnail.
TileRegion ShownRegion(const uint8_t* rgba, uint32_t w, uint32_t h, StereoLayout layout);

// The longest edge to make a w x h picture's thumbnail at, so that ShownRegion (taken with
// no bars) is at least `need` px on its shorter side, but no more than `slot` px on its
// longer side (the atlas slot it goes in), and never above the picture's own size or `cap`.
// A picture of unknown size (0 x 0) is taken as 16:9.
uint32_t ThumbnailEdge(uint32_t w, uint32_t h, StereoLayout layout, uint32_t need, uint32_t slot, uint32_t cap);

} // namespace mp::browser
