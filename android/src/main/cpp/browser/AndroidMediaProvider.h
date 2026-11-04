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

namespace mp::browser {

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
    void WantThumbnails(const std::vector<ThumbnailRequest>& wanted, uint32_t maxEdge) override;
    bool TakeThumbnail(Thumbnail& out) override;
    void Open(int64_t id) override;

    // The Java side, through the JNI calls (any thread).
    void SetAccess(MediaAccess access);
    void SetItems(std::vector<MediaItem> items);
    void AddThumbnail(Thumbnail thumbnail);
    bool TakeAccessRequest();
    bool TakeRefreshRequest();
    bool TakeWantedThumbnails(std::vector<ThumbnailRequest>& wanted, uint32_t& maxEdge);
    bool TakeOpenRequest(int64_t& id);

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
    uint32_t wantedEdge_ = 0;
    std::deque<Thumbnail> thumbnails_;
    bool openRequested_ = false;
    int64_t openId_ = 0;
};

} // namespace mp::browser
