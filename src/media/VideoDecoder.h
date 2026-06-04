// SPDX-License-Identifier: BSL-1.0
//
// VideoDecoder (M3) — FFmpeg decode of an SBS video on a background thread,
// converting each frame to RGBA (swscale) and publishing into a FrameRing. Loops at
// EOF. Per-OS hardware decode (VideoToolbox / D3D11VA+NVDEC / VAAPI) is auto-selected
// with a transparent download→upload step; software decode is the fallback. The
// decode thread / ring / upload path are unchanged from M2 — only the backend differs
// (and the choice of backend is invisible to the consumer: it still gets RGBA frames).
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

    // Active backend, for the HUD / logs. codecName_ is the FFmpeg codec (e.g. "hevc");
    // hwName_ is the hwaccel in use ("videotoolbox", "d3d11va", …) or "software".
    const char* CodecName() const { return codecName_; }
    const char* BackendName() const { return hwName_; }
    bool Hardware() const { return hardware_; }

    FrameRing& Ring() { return ring_; }

private:
    void DecodeLoop();
    // Attach a per-OS hwaccel device to the codec context. `decoder` is the FFmpeg
    // AVCodec* (void here so the header stays libav-free). Returns false → software.
    bool TryEnableHwAccel(const void* decoder);

    struct Impl;                  // FFmpeg state (defined in the .cpp)
    std::unique_ptr<Impl> impl_;

    FrameRing ring_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool open_ = false;
    int width_ = 0;
    int height_ = 0;

    // Backend reporting (pointers into FFmpeg's static name tables; safe to store).
    const char* codecName_ = "";
    const char* hwName_ = "software";
    bool hardware_ = false;
};

} // namespace mp
