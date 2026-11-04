// SPDX-License-Identifier: Apache-2.0
//
// MediaProvider: the device's media library, as the browser sees it. Each platform
// implements it (Android: AndroidMediaProvider over MediaStore). Render thread only: an
// implementation does its work on its own threads and hands results back through the
// Take* calls, which the browser makes once a frame.
#pragma once

#include "browser/MediaItem.h"

#include <cstdint>
#include <vector>

namespace mp::browser {

// Values cross JNI (MediaStoreProvider.kt): keep them in step.
enum class MediaAccess {
    Unknown = 0,  // not reported yet
    None = 1,     // not granted; RequestAccess() shows the system prompt
    Partial = 2,  // only the photos and videos the user picked (Android 14+)
    Full = 3,
    Denied = 4,   // refused for good; RequestAccess() opens the app's settings
};

struct Thumbnail {
    int64_t id = 0;
    uint32_t width = 0, height = 0;  // 0 x 0: no thumbnail could be made
    std::vector<uint8_t> rgba;
};

class MediaProvider {
public:
    virtual ~MediaProvider() = default;

    virtual MediaAccess Access() const = 0;
    // Ask the user for access, or for more of it when access is partial.
    virtual void RequestAccess() = 0;
    // Read the library again. The list arrives through TakeItems().
    virtual void Refresh() = 0;
    // The latest list, if a new one arrived since the last call.
    virtual bool TakeItems(std::vector<MediaItem>& out) = 0;
    // The thumbnails wanted, most wanted first, each to fit in maxEdge x maxEdge. Replaces
    // the previous list, so an id that drops out is not made. List only ids not yet received.
    virtual void WantThumbnails(const std::vector<int64_t>& ids, uint32_t maxEdge) = 0;
    // One finished thumbnail, if any.
    virtual bool TakeThumbnail(Thumbnail& out) = 0;
    // Hand the item to the player.
    virtual void Open(int64_t id) = 0;
};

} // namespace mp::browser
