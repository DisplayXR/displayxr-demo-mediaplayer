// SPDX-License-Identifier: Apache-2.0
//
// DepthWarp: a left and a right view from one image and its disparity map (#180), for
// one-view LIFs. Each eye forward-maps every row: the span between two neighbouring
// source pixels lands where their shifts put it and nearer spans win. A span that opens
// across a depth edge (a disocclusion) takes the farther pixel's colour.
#pragma once

#include "ImageDecoder.h"   // DecodedImage

#include <cstdint>
#include <vector>

namespace mp {

class DepthWarp {
public:
    // `depth` holds one byte per pixel of `image`, 255 = nearest. `parallax` is the total
    // left-right parallax between depth 0 and 255, as a fraction of the image width; the
    // middle depth stays at zero parallax. Returns the full-SBS pair (left half, right half).
    static DecodedImage MakeSbs(const DecodedImage& image, const std::vector<uint8_t>& depth,
                                float parallax);
};

} // namespace mp
