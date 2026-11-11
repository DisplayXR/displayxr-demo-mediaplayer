// SPDX-License-Identifier: Apache-2.0
//
// StereoClassifier: decides which grid items get the 3D badge, and how each thumbnail is
// packed (so the grid can show one eye). The player's own resolver assumes stereo unless
// mono is proven, which is right for playback but would badge every photo, so a badge
// needs positive evidence, cheapest first:
//   1. a verdict cached from an earlier run (same id, date modified and size)
//   2. the file name (`*_2x1`, `*_half_2x1`, `*_1x2`, TB tokens)
//   3. the file itself: a LIF or a two-track video (Thumbnail::file3D)
//   4. the content check (StereoDetect) on the thumbnail, side by side only. It takes
//      about 2 ms per thumbnail on the NP02J (11 ms in a debug build), too much for the
//      render thread with thumbnails arriving several a frame, so it runs on a worker.
// Verdicts persist in a small text file between runs, except a 2D one reached without the
// file check it needed (the file did not open): a LIF's thumbnail is one view, so that 2D
// may be wrong, and the next run looks again (#180).
#pragma once

#include "browser/MediaItem.h"
#include "browser/MediaProvider.h"
#include "media/StereoTypes.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace mp::browser {

struct StereoVerdict {
    bool known = false;   // false while the evidence is still coming in
    bool stereo = false;  // badge it
    // How the thumbnail is packed: Sbs*/Tb* show the left/top eye; Mono and Dual (a LIF or
    // two-track video, whose thumbnail is one view) show it whole.
    StereoLayout layout = StereoLayout::Mono;
};

class StereoClassifier {
public:
    StereoClassifier() = default;
    ~StereoClassifier();

    StereoClassifier(const StereoClassifier&) = delete;
    StereoClassifier& operator=(const StereoClassifier&) = delete;

    // `cachePath` is the classifier's own file of verdicts; it need not exist yet.
    bool Initialize(const std::string& cachePath);
    // Stops the worker and saves the cache.
    void Shutdown();

    // What is known about `item` without its thumbnail: the cache, then the file name.
    StereoVerdict Get(const MediaItem& item) const;
    // Whether the file itself is worth a look (ThumbnailRequest::checkFile): only while the
    // verdict is unknown, and only for a JPEG or .lif (a LIF) or a video (two tracks).
    bool NeedsFileCheck(const MediaItem& item) const;
    // Decide `item` with its thumbnail. The thumbnail comes back through TakeResult() with
    // the verdict: at once when cheap evidence decides, else after the content check.
    void Classify(const MediaItem& item, Thumbnail thumbnail);
    bool TakeResult(Thumbnail& thumbnail, StereoVerdict& verdict);
    // Write the cache if it changed.
    void Save();

private:
    struct Entry {
        int64_t modified = 0, bytes = 0;
        StereoVerdict verdict;
        bool save = true;
    };
    struct Job {
        MediaItem item;
        Thumbnail thumbnail;
        StereoVerdict verdict;
        bool unchecked = false;  // the file check was needed but did not happen
    };

    void Load();
    void Work();

    std::string cachePath_;
    std::unordered_map<int64_t, Entry> verdicts_;  // render thread
    bool dirty_ = false;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;  // for the worker
    std::deque<Job> done_;  // for TakeResult
    bool stop_ = false;
    std::thread worker_;
};

} // namespace mp::browser
