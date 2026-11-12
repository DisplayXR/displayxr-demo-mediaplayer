// SPDX-License-Identifier: Apache-2.0
//
// ThumbnailDepth: how deep each stereo thumbnail's subject sits, for the 3D grid's
// per-tile convergence (StereoGrid.h TileShift). The measurement is the player's own
// auto-convergence estimator (media/AutoConvergence.h: block matching on the two eyes'
// luma) run once per thumbnail on a worker thread: a few milliseconds each, too much for
// the render thread when several thumbnails arrive in a frame. Render thread API.
#pragma once

#include "browser/StereoGrid.h"
#include "media/StereoTypes.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mp::browser {

class ThumbnailDepth {
public:
    ThumbnailDepth() = default;
    ~ThumbnailDepth();

    ThumbnailDepth(const ThumbnailDepth&) = delete;
    ThumbnailDepth& operator=(const ThumbnailDepth&) = delete;

    bool Initialize();
    void Shutdown();

    // Measure a packed (side by side or top-bottom) w x h RGBA thumbnail of item `id`. The
    // pixels are copied. Ignored for an unpacked layout or an id already measured.
    void Measure(int64_t id, const uint8_t* rgba, uint32_t w, uint32_t h, StereoLayout layout);
    // Take the finished measurements (once a frame).
    void TakeResults();
    // The estimate for `id`; false while it is unmeasured (or was never asked for).
    bool Get(int64_t id, DepthEstimate& out) const;
    // An edited file: measure it again next time.
    void Forget(int64_t id);

private:
    static constexpr size_t kMaxKept = 1024;  // estimates are 12 bytes; forget the oldest past this

    struct Job {
        int64_t id = 0;
        uint32_t w = 0, h = 0;
        StereoLayout layout = StereoLayout::Mono;
        std::vector<uint8_t> rgba;
    };
    struct Done {
        int64_t id = 0;
        DepthEstimate estimate;
    };

    void Work();

    std::unordered_map<int64_t, DepthEstimate> estimates_;  // render thread
    std::unordered_map<int64_t, bool> asked_;               // render thread: queued or measured
    std::deque<int64_t> order_;                             // render thread: asked_, oldest first

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Done> done_;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace mp::browser
