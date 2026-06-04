// SPDX-License-Identifier: BSL-1.0
//
// VideoDecoder (M2) — FFmpeg software decode of an SBS video on a background
// thread, converting each frame to RGBA (swscale) and publishing into a FrameRing.
// Loops at EOF. Hardware decode (per-OS hwaccel) is M3; the decode thread / ring /
// upload path here stay the same — only the decoder backend changes.
//
// FFmpeg types are hidden behind a pimpl so this header carries no libav* include.
// Built without FFmpeg (MEDIAPLAYER_WITH_FFMPEG undefined), Open() simply fails.
#pragma once

#include "media/FrameRing.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace mp {

class VideoDecoder {
public:
    VideoDecoder();   // defined in .cpp (pimpl: Impl must be complete there)
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Open the file and start the decode thread. Returns false if FFmpeg is absent
    // or the file can't be opened/decoded.
    bool Open(const std::string& path);
    void Stop();

    bool IsOpen() const { return open_; }
    int Width() const { return width_; }    // full SBS frame width
    int Height() const { return height_; }

    FrameRing& Ring() { return ring_; }

private:
    void DecodeLoop();

    struct Impl;                  // FFmpeg state (defined in the .cpp)
    std::unique_ptr<Impl> impl_;

    FrameRing ring_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool open_ = false;
    int width_ = 0;
    int height_ = 0;
};

} // namespace mp
