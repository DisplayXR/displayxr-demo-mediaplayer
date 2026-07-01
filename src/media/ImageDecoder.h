// SPDX-License-Identifier: Apache-2.0
//
// ImageDecoder (M1) — stb_image for JPG/PNG. Produces one tightly-packed RGBA8
// buffer (the whole SBS frame) + dimensions. The SBS split happens later in the
// sampler (UV sub-region per eye), so the decoder stays format-agnostic.
// (Optional libheif behind MEDIAPLAYER_WITH_HEIF is a later addition.)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mp {

struct DecodedImage {
    std::vector<uint8_t> pixels;  // RGBA8, width*height*4, row-major top-down
    int width = 0;
    int height = 0;

    bool Valid() const { return !pixels.empty() && width > 0 && height > 0; }
};

class ImageDecoder {
public:
    // Load a JPG/PNG file as RGBA8. Returns an invalid image on failure (logged).
    static DecodedImage Load(const std::string& path);

    // Decode an in-memory JPG/PNG buffer as RGBA8 (e.g. a JPEG blob embedded in a
    // LIF container). `tag` is only used for log messages. Invalid image on failure.
    static DecodedImage LoadFromMemory(const uint8_t* data, size_t len,
                                       const char* tag = "<memory>");
};

} // namespace mp
