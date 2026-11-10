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
        queueDepth_ = depth <= 0 ? 0 : std::min(std::max(depth, 2), kMaxBuffers - 2);
        queueLen_ = 0;
        queueHead_ = 0;
        freeCount_ = 0;
        for (int i = 0; i < kMaxBuffers; ++i)
            if (i != producer_ && i != consumer_) free_[freeCount_++] = i;
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
        if (queueDepth_ > 0) {
            bool dropped = false;
            if (queueLen_ == queueDepth_) {  // full: drop the oldest waiting frame
                free_[freeCount_++] = queue_[queueHead_];
                queueHead_ = (queueHead_ + 1) % kMaxBuffers;
                --queueLen_;
                dropped = true;
            }
            queue_[(queueHead_ + queueLen_) % kMaxBuffers] = producer_;
            ++queueLen_;
            producer_ = free_[--freeCount_];
            everPublished_ = true;
            return dropped;
        }
        std::swap(producer_, middle_);
        const bool dropped = hasNew_;
        hasNew_ = true;
        return dropped;
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
    static constexpr int kMaxBuffers = 6;  // producer + consumer + up to 4 waiting

    const Frame* AcquireQueuedLocked() {
        if (queueLen_ == 0) {
            if (everPublished_) ++repeated_;
            NoteBacklogLocked(0);
            return nullptr;
        }
        free_[freeCount_++] = consumer_;
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
            free_[freeCount_++] = queue_[queueHead_];
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
};

} // namespace mp
