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

DecodedImage ImageDecoder::LoadFromMemory(const uint8_t* data, size_t len, const char* tag) {
    DecodedImage out;
    if (!data || len == 0) return out;
    int w = 0, h = 0, channels = 0;
    // Force 4 channels (RGBA), matching the file path so the Vulkan upload is uniform.
    stbi_uc* px = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 4);
    if (!px) {
        LOG_ERROR("ImageDecoder: failed to decode '%s' from memory: %s", tag,
                  stbi_failure_reason());
        return out;
    }
    out.width = w;
    out.height = h;
    out.pixels.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    LOG_INFO("ImageDecoder: decoded '%s' from memory (%dx%d, %d src channels)", tag, w, h,
             channels);
    return out;
}

} // namespace mp
