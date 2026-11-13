// SPDX-License-Identifier: Apache-2.0
//
// FrameRing (M2) — lock-light triple-buffered handoff of decoded video frames from
// the decode thread to the render thread. Decouples decode rate from display rate:
// the producer always fills a private buffer and publishes; the consumer always
// samples the latest published frame (dropping intermediates). Only a pointer swap
// happens under the lock — never a pixel copy.
//
// Frames carry planar YUV (the decoder's native output: I420 or NV12) so the GPU does
// the colour convert + downscale — no swscale on the decode thread.
//
// In queue mode the renderer's upload thread may pin the next waiting frame and copy it into
// GPU staging before the consumer takes it (PinNextQueued, rhi/AsyncUpload.h); a pinned
// buffer is never handed back to the producer until it is unpinned.
//
// Also in queue mode, a producer may RESERVE free buffers and fill them outside the
// WriteBuffer()/Publish() pair (Media Player Pro's DeckLink direct pairing: each eye's lane
// converts straight into its half of the side-by-side frame), then publish or cancel each.
// A reserved buffer is in no queue until published, so the consumer and the upload thread
// never see it half-written; nobody else uses the ring this way.
#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mp {

// P016: NV12 layout with 16-bit samples, MSB-aligned (P010 is the 10-bit case). Pro's
// 10-bit capture (DeckLink v210) publishes it; the base player never does.
enum class PixFormat { I420, NV12, RGBA, P016 };  // 3-plane, 2-plane, packed RGBA (zero-copy #28), 16-bit 2-plane

// Y'CbCr -> R'G'B' matrix of a YUV frame. The value is what the renderer pushes to the
// MP_GRADE shader (`yuvMatrix`), so BT709 must stay 0: every producer that never sets it
// gets today's BT.709 convert, bit for bit. BT2020 is the non-constant-luminance matrix.
enum class ColorMatrix : int { BT709 = 0, BT601 = 1, BT2020 = 2 };
// Transfer characteristic. The base player treats every frame as SDR display-referred;
// Media Player Pro maps HLG / PQ (and scene-linear EXR, which the decoder stores as PQ of
// linear x 203 nits so values above 1.0 survive) to the SDR panel (#134, color/ToneMap.h).
// Values match color::Transfer.
enum class ColorTransfer : int { SDR = 0, HLG = 1, PQ = 2, LinearPq = 3 };

// The usual rule when a stream does not say: SD (576 lines and below) is BT.601, HD+ BT.709.
inline ColorMatrix DefaultMatrixForHeight(int height) {
    return height > 0 && height <= 576 ? ColorMatrix::BT601 : ColorMatrix::BT709;
}
inline const char* ColorMatrixName(ColorMatrix m) {
    return m == ColorMatrix::BT601 ? "BT.601" : m == ColorMatrix::BT2020 ? "BT.2020" : "BT.709";
}
inline const char* ColorTransferName(ColorTransfer t) {
    return t == ColorTransfer::HLG        ? "HLG"
           : t == ColorTransfer::PQ       ? "PQ"
           : t == ColorTransfer::LinearPq ? "Linear"
                                          : "SDR";
}

class FrameRing {
public:
    struct Frame {
        // plane[0]=Y (width x height). I420: plane[1]=U, plane[2]=V (each w/2 x h/2).
        // NV12: plane[1]=interleaved UV (w/2 x h/2, 2 bytes/texel); plane[2] unused.
        std::vector<uint8_t> plane[3];
        int width = 0;                // luma width/height (full frame, both eyes for SBS)
        int height = 0;
        PixFormat format = PixFormat::I420;
        bool fullRange = false;       // JPEG/full vs MPEG/limited range
        ColorMatrix matrix = ColorMatrix::BT709;      // YUV matrix (ignored for RGBA)
        ColorTransfer transfer = ColorTransfer::SDR;  // Pro tone-maps HDR (#134)
        bool primaries2020 = false;   // BT.2020 colour primaries (HDR video); else BT.709
        float peakNits = 0.0f;        // the stream's brightest (MaxCLL / mastering), 0 = unknown
        int64_t serial = 0;           // monotonic; lets the consumer detect newness
        // Live sources: when the frame was captured (steady seconds, LiveProducer::
        // NowSeconds) and its source timecode ("HH:MM:SS:FF", empty when it has none).
        double captureSec = 0.0;
        char timecode[16] = {0};

        // Zero-copy interop (#28, Windows). When gpu==true the pixels live in a shared
        // D3D11 NV12 texture (below) instead of the CPU plane[] buffers; the decode thread
        // CopySubresourceRegion's the decoded surface into it and the renderer imports +
        // samples it. Persistent per slot, owned by the decoder. Coherence is a CPU
        // event-query wait before Publish() (no keyed mutex — the shared texture uses the
        // legacy D3D11_RESOURCE_MISC_SHARED KMT handle). plane[] stays empty in this mode.
        bool gpu = false;
        void* gpuSharedTexture = nullptr;  // ID3D11Texture2D* (per-slot, decoder-owned)
        void* gpuSharedHandle = nullptr;   // legacy KMT shared handle for Vulkan import
    };

    static constexpr int kBufferCount = 3;  // slots the latest-wins mode uses (the decoder's GPU slots)
    static constexpr int kMaxQueueDepth = 4;  // queue mode's deepest queue (SetQueueDepth clamps)
    static constexpr int kMaxReserved = 3;    // reserved buffers out at once (ReserveBuffer)
    Frame& BufferSlot(int i) { return buffers_[i]; }  // producer-side GPU resource mgmt

    // Queue mode (live sources): instead of latest-wins, published frames wait in a short
    // FIFO and the consumer takes ONE per call (one per display refresh). A source whose
    // clock runs a few ppm off the panel's then costs one clean drop or repeat every few
    // minutes, where latest-wins drops in bursts each time the two clocks' phases line up
    // (refresh jitter decides, frame by frame, whether a frame lands before or after the
    // vsync). Cost: the cushion of up to one frame that the queue keeps.
    //   depth  = how many frames may wait (the oldest is dropped past it), >= 2
    // Call before the producer starts; 0 = latest-wins (the default).
    void SetQueueDepth(int depth) {
        std::lock_guard<std::mutex> lk(mutex_);
        // At most kMaxQueueDepth: the producer's, the consumer's, a pinned one (below) and
        // kMaxReserved reserved ones must fit beside a full queue.
        queueDepth_ = depth <= 0 ? 0 : std::min(std::max(depth, 2), kMaxQueueDepth);
        queueLen_ = 0;
        queueHead_ = 0;
        freeCount_ = 0;
        for (int i = 0; i < kMaxBuffers; ++i)
            if (i != producer_ && i != consumer_ && i != pinned_ && !reserved_[i]) free_[freeCount_++] = i;
        // A pin held across this goes to the free list at Unpin, unless it is the consumer's
        // (that one is released when the consumer moves on, as any other).
        pinnedParked_ = pinned_ >= 0 && pinned_ != consumer_;
        windowAcquires_ = 0;
        windowMinBacklog_ = kMaxBuffers;
        hasNew_ = false;
    }
    bool QueueMode() const { return queueDepth_ > 0; }
    // Queue mode: frames discarded on the consumer side (a standing backlog trimmed back
    // to the cushion), and calls that found nothing new (the last frame is shown again).
    uint64_t TrimmedFrames() {
        std::lock_guard<std::mutex> lk(mutex_);
        return trimmed_;
    }
    uint64_t RepeatedFrames() {
        std::lock_guard<std::mutex> lk(mutex_);
        return repeated_;
    }

    // --- Producer (decode thread) ---
    // The buffer to fill, then Publish() to hand it to the consumer.
    Frame& WriteBuffer() { return buffers_[producer_]; }
    // Returns true when this publish replaced a frame the consumer never acquired (a
    // drop) — live sources (#93) count it; everyone else ignores the result.
    bool Publish() {
        std::lock_guard<std::mutex> lk(mutex_);
        buffers_[producer_].serial = ++serial_;
        bool dropped = false;
        if (queueDepth_ > 0) {
            if (queueLen_ == queueDepth_) {  // full: drop the oldest waiting frame
                ReleaseLocked(queue_[queueHead_]);
                queueHead_ = (queueHead_ + 1) % kMaxBuffers;
                --queueLen_;
                dropped = true;
            }
            queue_[(queueHead_ + queueLen_) % kMaxBuffers] = producer_;
            ++queueLen_;
            // never the pinned buffer (ReleaseLocked parks it) nor a reserved one (not free)
            producer_ = free_[--freeCount_];
            everPublished_ = true;
        } else {
            std::swap(producer_, middle_);
            dropped = hasNew_;
            hasNew_ = true;
        }
        if (publishHook_) publishHook_(publishHookCtx_);
        return dropped;
    }

    // --- Reserved buffers (queue mode; Pro's DeckLink direct pairing) ---
    // A free buffer the producer fills itself, possibly from several threads (each writing
    // its own part), then hands over with PublishReserved() or gives back with
    // CancelReserved(). It was free when taken, so neither the consumer nor a pin holds it,
    // and until it is published it is in no queue: AcquireLatest() never returns it and
    // PinNextQueued() never pins it. nullptr in latest-wins mode or with kMaxReserved out.
    // Never so many that Publish() could find no free buffer: kMaxBuffers counts the
    // producer's, the consumer's, a full queue (kMaxQueueDepth), a parked pin and
    // kMaxReserved, so a free one is always left for Publish().
    Frame* ReserveBuffer() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (queueDepth_ <= 0 || reservedCount_ >= kMaxReserved || freeCount_ <= 0) return nullptr;
        const int b = free_[--freeCount_];
        reserved_[b] = true;
        ++reservedCount_;
        return &buffers_[b];
    }
    // Publishes a reserved buffer as Publish() publishes WriteBuffer(): a new serial, the
    // oldest waiting frame dropped when the queue is full (returns true then), the publish
    // hook. WriteBuffer() is unchanged. The caller keeps all publishes (this and Publish())
    // on one thread at a time, as for Publish(). Nothing for a buffer that is not reserved.
    bool PublishReserved(Frame* f) {
        std::lock_guard<std::mutex> lk(mutex_);
        const int b = ReservedIndexLocked(f);
        if (b < 0) return false;
        reserved_[b] = false;
        --reservedCount_;
        if (queueDepth_ <= 0) return false;  // (latest-wins since the reserve: SetQueueDepth's caller's error)
        buffers_[b].serial = ++serial_;
        bool dropped = false;
        if (queueLen_ == queueDepth_) {  // full: drop the oldest waiting frame
            ReleaseLocked(queue_[queueHead_]);
            queueHead_ = (queueHead_ + 1) % kMaxBuffers;
            --queueLen_;
            dropped = true;
        }
        queue_[(queueHead_ + queueLen_) % kMaxBuffers] = b;
        ++queueLen_;
        everPublished_ = true;
        if (publishHook_) publishHook_(publishHookCtx_);
        return dropped;
    }
    // A reserved buffer back to the free list, unpublished (it was never anyone else's).
    void CancelReserved(Frame* f) {
        std::lock_guard<std::mutex> lk(mutex_);
        const int b = ReservedIndexLocked(f);
        if (b < 0) return;
        reserved_[b] = false;
        --reservedCount_;
        if (queueDepth_ > 0) free_[freeCount_++] = b;
    }
    int ReservedCount() {
        std::lock_guard<std::mutex> lk(mutex_);
        return reservedCount_;
    }

    // --- Pre-staging (the renderer's upload thread, rhi/AsyncUpload.h) ---
    // Queue mode only. The oldest waiting frame with a serial above `afterSerial` that has
    // CPU planes (not a zero-copy `gpu` frame), PINNED: until Unpin() its buffer is never
    // handed back to the producer. A drop of the oldest waiting frame, a trim, or the
    // consumer moving on parks it instead, and Unpin() frees it then. The consumer may still
    // acquire a pinned frame (both sides only read it), so the cadence, the drops, the trims
    // and the repeats are exactly those of an unpinned ring. One pin at a time: nullptr when
    // a pin is held, nothing qualifies, or the ring is latest-wins.
    const Frame* PinNextQueued(int64_t afterSerial) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (queueDepth_ <= 0 || pinned_ >= 0) return nullptr;
        for (int i = 0; i < queueLen_; ++i) {
            const int b = queue_[(queueHead_ + i) % kMaxBuffers];
            if (buffers_[b].serial > afterSerial && !buffers_[b].gpu) {
                pinned_ = b;
                pinnedParked_ = false;
                return &buffers_[b];
            }
        }
        return nullptr;
    }
    void Unpin(const Frame* f) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (pinned_ < 0 || f != &buffers_[pinned_]) return;
        if (pinnedParked_) free_[freeCount_++] = pinned_;  // released while pinned: free it now
        pinned_ = -1;
        pinnedParked_ = false;
    }
    bool IsPinned(const Frame* f) {
        std::lock_guard<std::mutex> lk(mutex_);
        return pinned_ >= 0 && f == &buffers_[pinned_];
    }
    // Called on the producer's thread, under the ring's lock, at the end of every Publish():
    // the pre-stager's wake-up, so keep it short and never call back into the ring. nullptr
    // removes it; once this returns, the previous hook is never called again.
    using PublishHook = void (*)(void* ctx);
    void SetPublishHook(PublishHook fn, void* ctx) {
        std::lock_guard<std::mutex> lk(mutex_);
        publishHook_ = fn;
        publishHookCtx_ = fn ? ctx : nullptr;
    }

    // --- Consumer (render thread) ---
    // Returns the latest published frame (owned until the next call), or nullptr if
    // nothing new since the last acquire. Queue mode: the oldest waiting frame instead.
    const Frame* AcquireLatest() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (queueDepth_ > 0) return AcquireQueuedLocked();
        if (!hasNew_) return nullptr;
        std::swap(consumer_, middle_);
        hasNew_ = false;
        return &buffers_[consumer_];
    }

private:
    // producer + consumer + up to 4 waiting + 1 pinned + up to 3 reserved. A buffer's planes
    // are allocated on its first use, so the ones a source never reaches cost nothing.
    static constexpr int kMaxBuffers = 3 + kMaxQueueDepth + kMaxReserved;

    int ReservedIndexLocked(const Frame* f) const {
        for (int b = 0; b < kMaxBuffers; ++b)
            if (f == &buffers_[b]) return reserved_[b] ? b : -1;
        return -1;
    }

    // A queue-mode buffer the ring is done with: back to the free list, or parked until
    // Unpin() when it is pinned (the upload thread is still copying it).
    void ReleaseLocked(int b) {
        if (b == pinned_) pinnedParked_ = true;
        else free_[freeCount_++] = b;
    }

    const Frame* AcquireQueuedLocked() {
        if (queueLen_ == 0) {
            if (everPublished_) ++repeated_;
            NoteBacklogLocked(0);
            return nullptr;
        }
        ReleaseLocked(consumer_);
        consumer_ = queue_[queueHead_];
        queueHead_ = (queueHead_ + 1) % kMaxBuffers;
        --queueLen_;
        NoteBacklogLocked(queueLen_);
        return &buffers_[consumer_];
    }
    // The backlog left after each take, over a window of kTrimWindow takes. One waiting
    // frame is the cushion that absorbs refresh jitter; when even the window's minimum
    // is two, the source has gained a whole frame on the display (its clock is faster),
    // so drop one to put the latency back. The opposite drift needs nothing: an empty
    // take repeats the last frame, which rebuilds the cushion.
    static constexpr int kTrimWindow = 120;  // ~2 s at 60 Hz
    void NoteBacklogLocked(int backlog) {
        windowMinBacklog_ = std::min(windowMinBacklog_, backlog);
        if (++windowAcquires_ < kTrimWindow) return;
        if (windowMinBacklog_ >= 2 && queueLen_ > 0) {
            ReleaseLocked(queue_[queueHead_]);
            queueHead_ = (queueHead_ + 1) % kMaxBuffers;
            --queueLen_;
            ++trimmed_;
        }
        windowAcquires_ = 0;
        windowMinBacklog_ = kMaxBuffers;
    }

    Frame buffers_[kMaxBuffers];
    std::mutex mutex_;
    int producer_ = 0;  // filled by the decode thread
    int middle_ = 1;    // most-recently-published handoff slot (latest-wins)
    int consumer_ = 2;  // held by the render thread
    bool hasNew_ = false;
    int64_t serial_ = 0;
    // Queue mode
    int queueDepth_ = 0;
    int queue_[kMaxBuffers] = {};
    int queueHead_ = 0, queueLen_ = 0;
    int free_[kMaxBuffers] = {};
    int freeCount_ = 0;
    bool everPublished_ = false;
    int windowAcquires_ = 0, windowMinBacklog_ = kMaxBuffers;
    uint64_t trimmed_ = 0, repeated_ = 0;
    // Pre-staging (queue mode): the pinned buffer, and whether the ring released it while
    // pinned (it goes to the free list at Unpin).
    int pinned_ = -1;
    bool pinnedParked_ = false;
    // Reserved buffers (queue mode): out of the free list, in no queue.
    bool reserved_[kMaxBuffers] = {};
    int reservedCount_ = 0;
    PublishHook publishHook_ = nullptr;
    void* publishHookCtx_ = nullptr;
};

} // namespace mp
