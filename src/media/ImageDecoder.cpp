// SPDX-License-Identifier: BSL-1.0
#include "ImageDecoder.h"

#include "Log.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace mp {

DecodedImage ImageDecoder::Load(const std::string& path) {
    DecodedImage out;
    int w = 0, h = 0, channels = 0;
    // Force 4 channels (RGBA) so the Vulkan upload is always a known format.
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        LOG_ERROR("ImageDecoder: failed to load '%s': %s", path.c_str(), stbi_failure_reason());
        return out;
    }
    out.width = w;
    out.height = h;
    out.pixels.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    LOG_INFO("ImageDecoder: loaded '%s' (%dx%d, %d src channels)", path.c_str(), w, h, channels);
    return out;
}

} // namespace mp
