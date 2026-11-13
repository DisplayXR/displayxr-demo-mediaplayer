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
#include <vector>

namespace mp::livepix {
class ConvertPool;  // pro/ConvertPool.h
struct EyeView;     // pro/ConvertPool.h
}

namespace mp {

class LiveSourceBase : public LiveProducer {
public:
    // Live frames go through the ring's queue mode (one frame per refresh, a one-frame
    // cushion): MEDIAPLAYER_LIVE_QUEUE=<depth> sets the depth, 0 = the old latest-wins.
    LiveSourceBase();
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
    // The 16-bit (P016) twin: 10-bit capture kept to the GPU. `pool` (the caller's convert
    // threads; nullptr = this thread) copies it in row bands (pro/ConvertPool.h).
    void Publish(const livepix::P016& f, bool fullRange, double captureSec, const char* timecode,
                 int eyeOffsetFrames, livepix::ConvertPool* pool = nullptr);
    // Two eyes composed side by side straight into the ring (no intermediate frame; a
    // 4K pair is 50 MB a copy, in row bands on `pool`). Sizes may differ: the smaller one is
    // used. The left eye's colour tags stand for the pair.
    void PublishPair(const livepix::P016& l, const livepix::P016& r, bool fullRange, double captureSec,
                     const char* timecode, int eyeOffsetFrames, livepix::ConvertPool* pool = nullptr);
    // The same from eye views (the left eye's tags given): the P016 one is this.
    void PublishPair(const livepix::EyeView& l, const livepix::EyeView& r, ColorMatrix matrix,
                     ColorTransfer transfer, bool fullRange, double captureSec, const char* timecode,
                     int eyeOffsetFrames, livepix::ConvertPool* pool = nullptr);

    // ---- Direct pairing (DeckLink two inputs): each eye converted straight into its half.
    // A ring frame for two eyes of eyeW x eyeH side by side, its planes sized as PublishPair
    // sizes them for that pair (2*eyeW x eyeH, eyeH even). Each eye's lane writes its half
    // (livepix::PairHalf), then PublishPairFrame. It is FrameRing::ReserveBuffer's: in no
    // queue until published, so the renderer and the upload thread never see it half done.
    // nullptr when the ring has none to spare (kMaxReserved out, or latest-wins).
    FrameRing::Frame* ReservePairFrame(int eyeW, int eyeH);
    // A reserved pair frame back to the ring, unpublished.
    void CancelPairFrame(FrameRing::Frame* f);
    // Publishes a reserved pair frame whose two halves are written, tagged exactly as
    // PublishPair tags a pair of two eyeW x eyeH eyes (the left eye's matrix and transfer).
    void PublishPairFrame(FrameRing::Frame* f, int eyeW, int eyeH, ColorMatrix matrix, ColorTransfer transfer,
                          bool fullRange, double captureSec, const char* timecode, int eyeOffsetFrames);
    // ---- Raw v210 (#238 phase 2, MEDIAPLAYER_DECKLINK_RAW_V210; media/V210.h): the card's rows
    // as they are, decoded on the GPU. eye[e] (e < eyes, 1 or 2) holds eye e's rows,
    // v210::RowBytes(eyeW) bytes each, h rows (at least that many bytes). They are MOVED into
    // the ring frame by swapping vectors, not copied: each eye[e] comes back holding the ring
    // buffer's previous plane, to refill (the same size after warm-up, so no allocation).
    // eyeW and h even. False (nothing published, nothing swapped) for a shape the renderer
    // would refuse. Only while v210::GpuDecodeActive().
    bool PublishV210(std::vector<uint8_t>* const eye[2], int eyes, int eyeW, int h, ColorMatrix matrix,
                     ColorTransfer transfer, bool fullRange, double captureSec, const char* timecode,
                     int eyeOffsetFrames);
    // The same with the rows left where they are (zero-copy, #238 phase 3): eye[e] is the host
    // block holding eye e's rows (FrameRing::Frame::ExtPlane: the block, and a hold on its
    // contents); the ring frame takes copies of both, and the renderer reads the block in place.
    // False (nothing published) for a shape the renderer would refuse.
    bool PublishV210Ext(const FrameRing::Frame::ExtPlane* const eye[2], int eyes, int eyeW, int h, ColorMatrix matrix,
                        ColorTransfer transfer, bool fullRange, double captureSec, const char* timecode,
                        int eyeOffsetFrames);
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
    // A two-eye frame's planes (w = both eyes, h even) and tags, one copy for the two paths.
    static void SizePairPlanes(FrameRing::Frame& f, int w, int h);
    static void TagPair(FrameRing::Frame& f, int w, int h, ColorMatrix matrix, ColorTransfer transfer,
                        bool fullRange, double captureSec, const char* timecode);
    mutable FrameRing ring_;  // mutable: GetStats reads its queue counters
    mutable std::mutex statsMutex_;
    LiveStats stats_;
    std::string lastError_;
    double openedAt_ = 0.0;
    double fpsWindowStart_ = 0.0;
    uint64_t fpsWindowCount_ = 0;
    uint64_t trimmedAtReset_ = 0, repeatedAtReset_ = 0;
};

}  // namespace mp
