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

struct ThumbnailRequest {
    int64_t id = 0;
    // Also look at the file itself for Thumbnail::file3D. It costs opening the file, so
    // ask only when the answer can matter (StereoClassifier::NeedsFileCheck).
    bool checkFile = false;
    // Make it to fit in maxEdge x maxEdge (TileCrop.h's ThumbnailEdge: what fills a tile).
    uint32_t maxEdge = 0;
    // The 3D grid: also hand the open file over (Thumbnail::file), to make the second view
    // of a 3D photo whose thumbnail is one view (StereoPairs.h).
    bool pairFile = false;
};

struct Thumbnail {
    int64_t id = 0;
    uint32_t width = 0, height = 0;  // 0 x 0: no thumbnail could be made
    std::vector<uint8_t> rgba;
    // The file itself is 3D: a LIF (two views, or one view and its depth map: every LIF
    // on the NP02J was one or the other) or a two-track (LVF) video. The thumbnail then
    // shows one view.
    bool file3D = false;
    // The file was read for file3D: asked for, and it opened.
    bool fileChecked = false;
    // Asked for with pairFile: the file, open (-1 if not), and its size. Whoever takes the
    // thumbnail owns the descriptor and closes it.
    int file = -1;
    uint64_t fileSize = 0;
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
    // The thumbnails wanted, most wanted first. Replaces the previous list, so an id that
    // drops out is not made. List only ids not yet received, or received smaller than the
    // maxEdge now asked for (a sharper one is then made).
    virtual void WantThumbnails(const std::vector<ThumbnailRequest>& wanted) = 0;
    // One finished thumbnail, with what the file says about its views, if any.
    virtual bool TakeThumbnail(Thumbnail& out) = 0;
    // Hand the item to the player.
    virtual void Open(int64_t id) = 0;
    // Whether Delete() can work here.
    virtual bool CanDelete() const = 0;
    // Delete the items once the user confirms; the platform asks. The new list arrives
    // through TakeItems().
    virtual void Delete(const std::vector<int64_t>& ids) = 0;
    // A short haptic tick, e.g. a long press starting select mode. Optional.
    virtual void Haptic() {}
};

} // namespace mp::browser
