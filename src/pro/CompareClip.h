// SPDX-License-Identifier: Apache-2.0
//
// pro::CompareClip — the B side of the clip-vs-clip wipe (Wipe::Clips): a second clip,
// decoded on its own thread, kept at the open clip's (A's) position, its frames handed to
// the renderer's compare planes (VulkanRenderer::UploadCompare*). A still B is decoded
// once. B plays on its own clock; FollowA() mirrors A's pause and corrects drift with a
// seek, so both sides of the wipe show the same moment. tests/compare_clip_test.cpp.
#pragma once

#include "media/VideoDecoder.h"

#include <memory>
#include <string>
#include <vector>

namespace mp {
class VulkanRenderer;
}

namespace mp::pro {

// What B should do to stay with A (pure; the policy FollowA applies).
struct SyncAction {
    bool pause = false;     // mirror A's pause
    double seekTo = -1.0;   // B timestamp to seek to, or < 0 for none
};
// posA / startA: A's frame time and first timestamp; the same for B. fps: B's rate (0 =
// unknown, 24 assumed). nowSec / lastSeekSec: wall clock, for the drift-seek hold-off.
// Paused, B is pinned to A's frame (within half a frame); playing, a drift over two frames
// is corrected at most every 0.5 s (a seek restarts B's clock; seeking every frame would
// stutter).
inline SyncAction DecideSync(double posA, double startA, bool pausedA, double posB, double startB, double fps,
                             double nowSec, double lastSeekSec) {
    SyncAction a;
    a.pause = pausedA;
    const double frame = 1.0 / (fps > 0.0 ? fps : 24.0);
    const double target = startB + (posA - startA);  // A's moment, in B's timestamps
    const double drift = posB - target;
    const double d = drift < 0.0 ? -drift : drift;
    if (pausedA) {
        if (d > 0.5 * frame) a.seekTo = target;
    } else if (d > 2.0 * frame && nowSec - lastSeekSec >= 0.5) {
        a.seekTo = target;
    }
    return a;
}

class CompareClip {
public:
    ~CompareClip() { Close(); }
    // Open B. False when it is neither a video nor an image this player decodes.
    bool Open(const std::string& path, std::string* err = nullptr);
    void Close();
    bool IsOpen() const { return open_; }
    const std::string& Path() const { return path_; }

    // Per frame: keep B with A (A's position / first timestamp / pause), then upload B's
    // newest frame (or the still, once) to `renderer`'s compare planes.
    void FollowA(double posA, double startA, bool pausedA, VulkanRenderer* renderer, double nowSec);

private:
    std::string path_;
    bool open_ = false, isStill_ = false, stillUploaded_ = false;
    std::unique_ptr<VideoDecoder> video_;
    std::vector<uint8_t> still_;
    int stillW_ = 0, stillH_ = 0;
    double lastSeekSec_ = -10.0;
    double lastSeekTo_ = -1.0;  // a seek still being decoded is not re-sent every frame
};

}  // namespace mp::pro
