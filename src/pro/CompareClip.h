// SPDX-License-Identifier: Apache-2.0
//
// pro::CompareClip — the B side of the clip-vs-clip wipe (Wipe::Clips): a second clip,
// decoded on its own thread, kept at the open clip's (A's) position, its frames handed to
// the renderer's compare planes (VulkanRenderer::UploadCompare*). A still B is decoded
// once. B is paced by A's clock (its decoder's master clock is A's position, mapped into
// B's timestamps), so it stays frame-locked to A without seeking; FollowA() mirrors A's
// pause and seeks B only on a jump: A's own seek or loop wrap (#250: B on the wall clock
// drifted, and the drift seeks every 0.5 s each held a frame). B never loops by itself.
// tests/compare_clip_test.cpp.
#pragma once

#include "media/VideoDecoder.h"

#include <atomic>
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
// unknown, 24 assumed). nowSec / lastSeekSec: wall clock, for the seek hold-off. endB: B's
// last timestamp (start + duration; <= 0 = unknown).
// Paused, B is pinned to A's frame (within half a frame). Playing, B is paced by A's
// clock, so small drift is not B's to fix: it seeks (at most every 0.5 s) only on a jump -
// ahead of A by over two frames (A seeked back or wrapped; pacing would only hold B's
// frame; a quarter second in B's last second), or behind by over a second (A seeked
// forward; catching up by decoding would take too long). A target past B's end is not
// chased: B holds its last frame.
inline SyncAction DecideSync(double posA, double startA, bool pausedA, double posB, double startB, double fps,
                             double nowSec, double lastSeekSec, double endB = -1.0) {
    SyncAction a;
    a.pause = pausedA;
    const double frame = 1.0 / (fps > 0.0 ? fps : 24.0);
    const double target = startB + (posA - startA);  // A's moment, in B's timestamps
    const double drift = posB - target;
    const double d = drift < 0.0 ? -drift : drift;
    const bool pastEnd = endB > 0.0 && target >= endB - frame;
    // In B's last second (by its container duration, which can run past the last frame)
    // its decoder drains its buffered frames on its own wall
    // clock, so they can run up to a quarter second ahead of A: not a jump.
    const double aheadTol = (endB > 0.0 && target >= endB - 1.0) ? 0.25 : 2.0 * frame;
    if (pausedA) {
        if (d > 0.5 * frame && !(pastEnd && drift < 0.0)) a.seekTo = target;
    } else if ((drift > aheadTol || (drift < -1.0 && !pastEnd)) && nowSec - lastSeekSec >= 0.5) {
        a.seekTo = target;
    }
    return a;
}

// B's hold counter (#250): gaps between new B frames while A plays, summarised and reset
// every `period` seconds. Pure, so the soak and the test read the same numbers.
struct HoldStats {
    int holds = 0;          // gaps over the threshold this period
    double longest = 0.0;   // longest gap this period, seconds
    int seeks = 0;          // seeks sent to B this period
    double lastFrame = -1.0, periodStart = -1.0;
    // A new B frame arrived at `now` (A playing). Returns true when the gap was a hold.
    bool Frame(double now, double threshold = 0.25) {
        bool hold = false;
        if (lastFrame >= 0.0) {
            const double gap = now - lastFrame;
            if (gap > threshold) { ++holds; hold = true; }
            if (gap > longest) longest = gap;
        }
        lastFrame = now;
        return hold;
    }
    void Break() { lastFrame = -1.0; }  // A paused, or B seeked by A's own jump: not B's hold
};

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
    // B's master clock: A's position in B's timestamps, written per frame by FollowA, read
    // by B's decode thread. < 0 = none (B falls back to its wall clock until its next seek).
    std::atomic<double> clockB_{-1.0};
    HoldStats holds_;
    void LogHolds(double nowSec);
    std::vector<uint8_t> still_;
    int stillW_ = 0, stillH_ = 0;
    double lastSeekSec_ = -10.0;
    double lastSeekTo_ = -1.0;  // a seek still being decoded is not re-sent every frame
};

}  // namespace mp::pro
