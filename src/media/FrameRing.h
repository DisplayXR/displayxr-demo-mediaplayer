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
// Transfer characteristic, carried as a TAG ONLY (for the UI): the renderer treats every
// frame as SDR display-referred. HLG / PQ are not tone-mapped (docs/PRO.md).
enum class ColorTransfer : int { SDR = 0, HLG = 1, PQ = 2 };

// The usual rule when a stream does not say: SD (576 lines and below) is BT.601, HD+ BT.709.
inline ColorMatrix DefaultMatrixForHeight(int height) {
    return height > 0 && height <= 576 ? ColorMatrix::BT601 : ColorMatrix::BT709;
}
inline const char* ColorMatrixName(ColorMatrix m) {
    return m == ColorMatrix::BT601 ? "BT.601" : m == ColorMatrix::BT2020 ? "BT.2020" : "BT.709";
}
inline const char* ColorTransferName(ColorTransfer t) {
    return t == ColorTransfer::HLG ? "HLG" : t == ColorTransfer::PQ ? "PQ" : "SDR";
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
        ColorTransfer transfer = ColorTransfer::SDR;  // tag only - never tone-mapped
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

    static constexpr int kBufferCount = 3;
    Frame& BufferSlot(int i) { return buffers_[i]; }  // producer-side GPU resource mgmt

    // --- Producer (decode thread) ---
    // The buffer to fill, then Publish() to hand it to the consumer.
    Frame& WriteBuffer() { return buffers_[producer_]; }
    // Returns true when this publish replaced a frame the consumer never acquired (a
    // drop) — live sources (#93) count it; everyone else ignores the result.
    bool Publish() {
        std::lock_guard<std::mutex> lk(mutex_);
        buffers_[producer_].serial = ++serial_;
        std::swap(producer_, middle_);
        const bool dropped = hasNew_;
        hasNew_ = true;
        return dropped;
    }

    // --- Consumer (render thread) ---
    // Returns the latest published frame (owned until the next call), or nullptr if
    // nothing new since the last acquire.
    const Frame* AcquireLatest() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!hasNew_) return nullptr;
        std::swap(consumer_, middle_);
        hasNew_ = false;
        return &buffers_[consumer_];
    }

private:
    Frame buffers_[3];
    std::mutex mutex_;
    int producer_ = 0;  // filled by the decode thread
    int middle_ = 1;    // most-recently-published handoff slot
    int consumer_ = 2;  // held by the render thread
    bool hasNew_ = false;
    int64_t serial_ = 0;
};

} // namespace mp
