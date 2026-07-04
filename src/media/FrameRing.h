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

enum class PixFormat { I420, NV12 };  // 3-plane (Y,U,V) or 2-plane (Y, interleaved UV)

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
        int64_t serial = 0;           // monotonic; lets the consumer detect newness

        // Zero-copy interop (#28, Windows). When gpu==true the pixels live in a shared
        // D3D11 NV12 texture (below) instead of the CPU plane[] buffers; the decode thread
        // CopySubresourceRegion's the decoded surface into it and the renderer imports +
        // samples it. Persistent per slot, owned by the decoder; synced by a keyed mutex
        // (key 0). plane[] stays empty in this mode.
        bool gpu = false;
        void* gpuSharedTexture = nullptr;  // ID3D11Texture2D* (per-slot, decoder-owned)
        void* gpuSharedHandle = nullptr;   // shared NT HANDLE for Vulkan import
    };

    static constexpr int kBufferCount = 3;
    Frame& BufferSlot(int i) { return buffers_[i]; }  // producer-side GPU resource mgmt

    // --- Producer (decode thread) ---
    // The buffer to fill, then Publish() to hand it to the consumer.
    Frame& WriteBuffer() { return buffers_[producer_]; }
    void Publish() {
        std::lock_guard<std::mutex> lk(mutex_);
        buffers_[producer_].serial = ++serial_;
        std::swap(producer_, middle_);
        hasNew_ = true;
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
