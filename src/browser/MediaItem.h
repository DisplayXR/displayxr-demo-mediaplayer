// SPDX-License-Identifier: Apache-2.0
//
// MediaItem: one photo or video in the device's media library, as a MediaProvider
// reports it.
#pragma once

#include <cstdint>
#include <string>

namespace mp::browser {

// Values cross JNI (MediaStoreProvider.kt): keep them in step.
enum class MediaType { Image = 0, Video = 1 };

struct MediaItem {
    int64_t id = 0;          // the provider's id (the MediaStore _ID on Android)
    std::string name;        // display name, e.g. "IMG_0042_2x1.jpg"
    MediaType type = MediaType::Image;
    int64_t dateMs = 0;      // when it was taken, else when last modified: the sort key
    int64_t modified = 0;    // last modified, seconds since the epoch
    int64_t bytes = 0;
    int64_t durationMs = 0;  // videos only
    std::string folder;      // its path in shared storage, e.g. "DCIM/Camera/"
    uint32_t width = 0, height = 0;  // px, upright (rotation applied); 0 x 0 if unknown
    // Desktop (#252). The provider-declared sources it belongs to: bit i = Source::bit i
    // (MediaLibrary::SetSources). Unused with the built-in (Android) sources.
    uint64_t sources = 0;
    // Its place in a source sorted by the provider (Recents: 0 = opened last).
    int order = 0;
    // A cloud placeholder: reading its bytes would download it, so the browser never does.
    bool cloudOnly = false;
};

} // namespace mp::browser
