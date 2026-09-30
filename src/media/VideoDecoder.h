// SPDX-License-Identifier: Apache-2.0
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
//
// Live mode (#93, OpenLive): a network stream URL (RTSP / RTMP / UDP / RTP / HTTP /
// HLS). Connecting happens on the decode thread, so OpenLive returns at once and the
// caller follows GetStreamState(). Live semantics like the camera: no PTS sleeping
// (every decoded frame is published as it arrives; the ring keeps only the newest),
// no seek / loop / duration, audio packets DROPPED (v1: a live feed never has an A/V
// clock to pace to), and automatic reconnect with 1-2-4-8 s backoff after a loss.
// Zero-copy interop (#28) is off for streams: a reconnect rebuilds the whole decoder,
// and the renderer's imports of the old device's shared textures must not dangle.
#pragma once

#include "media/FrameRing.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

    // ---- Live network stream (#93) ------------------------------------------------
    enum class StreamState { Idle, Connecting, Streaming, Reconnecting, Failed };
    static const char* StreamStateName(StreamState s);
    struct StreamStats {
        int width = 0, height = 0;   // of the last decoded frame
        float fpsIn = 0.0f;          // frames decoded per second (1 s window)
        float kbps = 0.0f;           // demuxed bitrate, all streams (1 s window)
        uint64_t frames = 0;         // decoded + published this session
        uint64_t dropped = 0;        // published over a frame the renderer never took
        int reconnects = 0;          // times the stream came back after a loss
        double connectMs = 0.0;      // open -> first frame of the current connection
        std::string lastError;       // last connect / read failure ("" = none)
        std::string codec, backend;  // "h264", "d3d11va" / "software"
    };
    // Start connecting to `url` on the decode thread (returns false only when FFmpeg is
    // absent). Stop() ends it. Any URL string FFmpeg accepts works; callers classify
    // first (media/StreamUrl.h) so SRT & co. are refused with a proper message.
    bool OpenLive(const std::string& url);
    bool IsLive() const { return live_; }
    bool Seekable() const { return !live_; }
    StreamState GetStreamState() const { return streamState_.load(); }
    StreamStats GetStreamStats() const;

    // Play/pause: when paused the decode thread holds the current frame and the
    // presentation clock is frozen, so resume continues seamlessly (no fast-forward).
    void SetPaused(bool p) { paused_.store(p); }
    void TogglePaused() { paused_.store(!paused_.load()); }
    bool Paused() const { return paused_.load(); }

    // Loop toggle. When off (default), the decoder holds the last frame at EOF and
    // reports Ended(); when on, it seeks back to the start. Read by the decode thread.
    void SetLoop(bool v) { loopEnabled_.store(v); }
    void ToggleLoop() { loopEnabled_.store(!loopEnabled_.load()); }
    bool Loop() const { return loopEnabled_.load(); }
    bool Ended() const { return ended_.load(); }   // reached EOF with loop disabled

    // Seek to `seconds` (clamped to [0, duration]). Thread-safe: the decode thread
    // performs the actual av_seek_frame on its next iteration, even while paused.
    // `preview` (during a scrub drag) shows the nearest keyframe — fast on high-res
    // long-GOP clips; the default exact seek decodes forward to the precise frame.
    void Seek(double seconds, bool preview = false);
    double PositionSeconds() const { return positionSec_.load(); }   // current frame PTS
    double DurationSeconds() const { return durationSec_; }           // 0 if unknown
    double FrameRate() const { return frameRate_; }                  // fps, 0 if unknown

    // Optional A/V master clock (audio position in seconds, or <0 when unavailable). When
    // set and returning >=0, the decode thread presents each frame when the clock reaches
    // its PTS instead of using its own wall clock. Set once before Open().
    void SetMasterClock(std::function<double()> fn) { masterClock_ = std::move(fn); }

#if defined(_WIN32)
    // Pin the D3D11VA decode device to a specific adapter — the Vulkan physical device's
    // LUID — so decoded surfaces can be shared into Vulkan without a CPU round trip
    // (zero-copy interop, issue #28). Call before Open(); the 8-byte LUID is copied.
    void SetInteropAdapterLUID(const uint8_t* luid8);
#endif

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
    // Live (#93): the connect / read / reconnect loop, run from DecodeLoop. `fill` is
    // DecodeLoop's frame->ring copier (AVFrame* passed as void*, header stays libav-free);
    // `timeBase` is re-pointed at each connection's video stream.
    void LiveLoop(const std::function<double(void*)>& fill, double& timeBase);
    // Open impl_->fmt on streamUrl_ with the low-latency options + the video decoder.
    // Returns false with `err` set; `fatal` = retrying cannot help (no such protocol /
    // demuxer / decoder, no video stream, HTTP 4xx).
    bool OpenLiveInput(std::string& err, bool& fatal);
    // Codec context for impl_->videoStream (hwaccel first, software fallback) —
    // shared by Open() and OpenLiveInput(). `label` is only for logs.
    bool OpenVideoCodec(const void* decoder, const std::string& label);
    // AVIOInterruptCB: aborts blocking network I/O on Stop() or when ioDeadlineNs_ passes.
    static int InterruptCallback(void* self);
    void SetStreamState(StreamState s) { streamState_.store(s); }
    // Attach a per-OS hwaccel device to the codec context. `decoder` is the FFmpeg
    // AVCodec* (void here so the header stays libav-free). Returns false → software.
    bool TryEnableHwAccel(const void* decoder);

    struct Impl;                  // FFmpeg state (defined in the .cpp)
    std::unique_ptr<Impl> impl_;

    FrameRing ring_;
    std::function<double()> masterClock_;   // audio clock for A/V sync (null = wall clock)
#if defined(_WIN32)
    uint8_t interopLUID_[8] = {};   // target adapter for zero-copy D3D11VA decode (#28)
    bool haveInteropLUID_ = false;
    bool interopActive_ = false;    // decoding on the pinned adapter (zero-copy eligible)
#endif
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> loopEnabled_{false};     // off by default: play once, hold last frame
    std::atomic<bool> ended_{false};           // hit EOF with looping disabled
    std::atomic<double> seekRequest_{-1.0};   // target seconds, <0 = none pending
    std::atomic<bool> seekPreview_{false};    // current seek wants the nearest keyframe
    std::atomic<double> positionSec_{0.0};     // last published frame's PTS

    // Live stream (#93). streamUrl_ is written before the thread starts; the stats block
    // is written by the decode thread and read by the UI under statsMutex_.
    bool live_ = false;
    std::string streamUrl_;
    std::atomic<StreamState> streamState_{StreamState::Idle};
    std::atomic<int64_t> ioDeadlineNs_{0};      // steady-clock ns; 0 = no deadline
    mutable std::mutex statsMutex_;
    StreamStats stats_;
    double durationSec_ = 0.0;                  // set in Open()
    double frameRate_ = 0.0;                    // fps, for frame stepping
    bool open_ = false;
    int width_ = 0;
    int height_ = 0;

    // Backend reporting (pointers into FFmpeg's static name tables; safe to store).
    const char* codecName_ = "";
    const char* hwName_ = "software";
    bool hardware_ = false;
};

} // namespace mp
