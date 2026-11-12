// SPDX-License-Identifier: Apache-2.0
//
// AndroidMediaProvider: the media browser's MediaProvider over the Android MediaStore
// (#160, docs/BROWSER.md). MediaStoreProvider.kt does the work on Java threads; the two
// halves meet in this object's queues. Java pushes results through the JNI calls in the
// .cpp and polls for requests once per Choreographer frame. Native never calls into Java:
// a JNI call into an app class from a native thread is classloader-fragile (runtime#507).
#pragma once

#include "browser/MediaProvider.h"

#include <deque>
#include <mutex>
#include <string>

namespace mp::browser {

// What a still of an item is made from: an image's file, to decode as the player does, or
// a video's first frame.
struct StillSource {
    int64_t id = 0;
    std::string name;
    int fd = -1;  // an image: the open file, which the taker closes
    uint32_t width = 0, height = 0;  // a video: its first frame, RGBA, or 0 x 0 if none
    std::vector<uint8_t> rgba;
};

class AndroidMediaProvider : public MediaProvider {
public:
    // The one instance the JNI calls reach.
    static AndroidMediaProvider& Instance();

    AndroidMediaProvider(const AndroidMediaProvider&) = delete;
    AndroidMediaProvider& operator=(const AndroidMediaProvider&) = delete;

    // MediaProvider (render thread).
    MediaAccess Access() const override;
    void RequestAccess() override;
    void Refresh() override;
    bool TakeItems(std::vector<MediaItem>& out) override;
    void WantThumbnails(const std::vector<ThumbnailRequest>& wanted) override;
    bool TakeThumbnail(Thumbnail& out) override;
    void Open(int64_t id) override;
    // createDeleteRequest needs Android 11.
    bool CanDelete() const override;
    void Delete(const std::vector<int64_t>& ids) override;

    // The player's NeighbourStills (render thread). The stills wanted (the files either side
    // of the one playing), each video frame to fit in maxEdge x maxEdge. Replaces the previous
    // list. They arrive through TakeStill().
    void WantStills(const std::vector<int64_t>& ids, uint32_t maxEdge);
    bool TakeStill(StillSource& out);

    // The Java side, through the JNI calls (any thread).
    void SetAccess(MediaAccess access);
    void SetItems(std::vector<MediaItem> items);
    void AddThumbnail(Thumbnail thumbnail);
    bool TakeAccessRequest();
    bool TakeRefreshRequest();
    bool TakeWantedThumbnails(std::vector<ThumbnailRequest>& wanted);
    bool TakeOpenRequest(int64_t& id);
    bool TakeDeleteRequest(std::vector<int64_t>& ids);
    void AddStill(StillSource still);
    bool TakeWantedStills(std::vector<int64_t>& ids, uint32_t& maxEdge);

private:
    AndroidMediaProvider() = default;

    mutable std::mutex mutex_;
    MediaAccess access_ = MediaAccess::Unknown;
    bool accessRequested_ = false;
    bool refreshRequested_ = false;
    bool itemsChanged_ = false;
    std::vector<MediaItem> items_;
    bool wantedChanged_ = false;
    std::vector<ThumbnailRequest> wanted_;
    std::deque<Thumbnail> thumbnails_;
    bool openRequested_ = false;
    int64_t openId_ = 0;
    std::vector<int64_t> deleteIds_;  // empty: no request
    bool stillsChanged_ = false;
    std::vector<int64_t> stillIds_;
    uint32_t stillEdge_ = 0;
    std::deque<StillSource> stills_;
};

} // namespace mp::browser
