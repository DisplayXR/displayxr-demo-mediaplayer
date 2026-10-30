// SPDX-License-Identifier: Apache-2.0
//
// LiveSourceBase — the shared half of a live source that builds whole NV12 frames on
// its own thread (DeckLinkSource, TestSignalSource): state, last error, name, the
// FrameRing and the delivered-fps / drop statistics, plus the stall watchdog.
#pragma once

#include "pro/LivePixels.h"
#include "media/LiveProducer.h"

#include <atomic>
#include <mutex>
#include <string>

namespace mp {

class LiveSourceBase : public LiveProducer {
public:
    LiveState GetState() const override { return state_.load(); }
    std::string LastError() const override;
    const std::string& DeviceName() const override { return name_; }
    LiveStats GetStats() const override;
    FrameRing& Ring() override { return ring_; }

protected:
    void Fail(LiveState s, const std::string& why);
    void SetState(LiveState s) { state_ = s; }
    void ResetStats(int fpsReq, double fps);
    // Copy a finished frame into the ring and publish it. timecode may be null.
    void Publish(const livepix::Nv12& f, bool fullRange, double captureSec, const char* timecode,
                 int eyeOffsetFrames);
    // The 16-bit (P016) twin: 10-bit capture kept to the GPU.
    void Publish(const livepix::P016& f, bool fullRange, double captureSec, const char* timecode,
                 int eyeOffsetFrames);
    // Two eyes composed side by side straight into the ring (no intermediate frame; a
    // 4K pair is 50 MB a copy). Sizes may differ: the smaller one is used. The left eye's
    // colour tags stand for the pair.
    void PublishPair(const livepix::P016& l, const livepix::P016& r, bool fullRange, double captureSec,
                     const char* timecode, int eyeOffsetFrames);
    // A frame the source had to skip before converting it (LiveStats::lateFrames).
    void CountLate();
    // Streaming (or Opening) -> Stalled when nothing was published for stallSec since
    // the last frame (or since the open): an input with no cable never streams at all.
    void CheckStall(double stallSec);
    void SetFps(double fps);
    // An input reports no signal (DeckLink's bmdFrameHasNoInputSource, a missing right
    // eye in dual-stream 3D): shown in the stats; the stall watchdog does the state.
    void SetNoSignal(int eye, bool on);

    std::string name_;
    std::atomic<LiveState> state_{LiveState::Closed};

private:
    void Published(int w, int h, bool fullRange, int eyeOffsetFrames, bool dropped);
    FrameRing ring_;
    mutable std::mutex statsMutex_;
    LiveStats stats_;
    std::string lastError_;
    double openedAt_ = 0.0;
    double fpsWindowStart_ = 0.0;
    uint64_t fpsWindowCount_ = 0;
};

}  // namespace mp
