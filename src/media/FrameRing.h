// SPDX-License-Identifier: BSL-1.0
//
// FrameRing (M2) — lock-light triple-buffered handoff of decoded RGBA frames from
// the decode thread to the render thread. Decouples decode rate from display rate:
// the producer always fills a private buffer and publishes; the consumer always
// samples the latest published frame (dropping intermediates). Only a pointer swap
// happens under the lock — never a pixel copy.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace mp {

class FrameRing {
public:
    struct Frame {
        std::vector<uint8_t> pixels;  // RGBA8, width*height*4
        int width = 0;
        int height = 0;
        int64_t serial = 0;           // monotonic; lets the consumer detect newness
    };

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
