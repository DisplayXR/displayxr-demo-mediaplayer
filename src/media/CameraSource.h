// SPDX-License-Identifier: Apache-2.0
//
// CameraSource (#90) — a live camera (UVC webcam, HDMI/SDI capture box) as a frame
// producer for the existing FrameRing. The renderer cannot tell it from a decoded
// video: it publishes tight NV12 planes exactly like VideoDecoder's CPU path, so the
// same UploadYUV + SBS-half/full slicing + eye swap apply unchanged.
//
// Backend: SDL3's camera API, requesting NV12. On Windows the Camera Frame Server
// decodes an MJPG-only device (the SpatialLabs Eyes) to NV12 in its own process —
// measured 60.0 fps at 3840x2160 with ~21 ms capture->app (see the #90 design doc).
//
// Threads:
//   - SDL's own camera thread reads samples and queues up to 8 surfaces.
//   - One worker thread (ours) opens the device (so a ~100-300 ms Media Foundation
//     activation never hitches a rendered frame), then drains SDL's queue to the
//     NEWEST surface and row-copies it into FrameRing::WriteBuffer(). Buffers keep
//     their capacity, so after warm-up a frame costs zero allocations.
//   - The main thread enumerates (lazily: SDL's camera subsystem is initialised on the
//     first Enumerate(), never by a file-only session), opens, stops and polls state.
//
// Safety: CameraSelect's denylist is enforced here too — Open() refuses a device whose
// name is on it, whatever the caller did. The panel's eye-tracking camera is NEVER
// opened (doing so would steal its stream from the eye tracker).
#pragma once

#include "media/CameraSelect.h"
#include "media/FrameRing.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp {

class MjpegPool;  // #90 follow-up: app-side MJPG decode for MJPG-only devices

class CameraSource {
public:
    enum class State {
        Closed,
        Opening,
        Streaming,
        Stalled,  // opened, but no frame for 2 s (e.g. a capture box with no signal)
        Lost,     // unplugged / device error
        Denied,   // OS privacy setting or a declined permission prompt
        Failed,   // could not open (LastError() says why)
    };
    struct Stats {
        float deliveredFps = 0.f;     // frames published to the ring per second (1 s window)
        uint64_t published = 0;
        uint64_t dropped = 0;         // SDL surfaces drained-and-discarded (we only show the newest)
        double lastPublishSec = 0.0;  // steady_clock seconds of the last Publish (consumer computes age)
        int w = 0, h = 0, fpsReq = 0;
        bool fullRange = false;
        bool mjpgPath = false;        // MJPG decode rung in use (#90 follow-up; always false today)
        int decoders = 0;             // decoder-pool size when mjpgPath
    };

    CameraSource() = default;
    ~CameraSource();  // Stop()
    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    // Main thread. Lazily SDL_InitSubSystem(SDL_INIT_CAMERA), then list the devices, with
    // denylisted ones flagged `denied`. `rescan` re-inits the subsystem (SDL's Media
    // Foundation driver has no hotplug) — skipped while a camera is open, since it would
    // pull the device out from under the worker.
    static std::vector<CameraDevice> Enumerate(bool rescan = false);
    // Print devices + formats for --camera-list. Denied devices are shown "[blocked]".
    static void PrintDeviceList(FILE* out);
    // Seconds on the steady clock (the timebase of Stats::lastPublishSec).
    static double NowSeconds();

    // Main thread. Non-blocking: validates the device (never a denied one) and picks a
    // format, then spawns the worker, which does SDL_OpenCamera. GetState() then goes
    // Opening -> Streaming | Failed | Denied. wantW/H/fps = 0 -> ChooseFormat defaults
    // (for the Eyes: NV12 3840x2160@60). Returns false (state Failed, LastError() set)
    // when the device is refused or offers nothing this build can consume.
    bool Open(const CameraDevice& dev, int wantW, int wantH, int wantFps);
    void Stop();  // join the worker (it closes the SDL camera); idempotent

    State GetState() const { return state_.load(); }
    std::string LastError() const;
    const std::string& DeviceName() const { return name_; }
    uint32_t DeviceId() const { return id_; }
    int Width() const { return w_.load(); }
    int Height() const { return h_.load(); }
    int Fps() const { return fpsReq_; }
    Stats GetStats() const;
    // App forwards SDL_EVENT_CAMERA_DEVICE_REMOVED for our id -> Lost.
    void NotifyRemoved() { removed_ = true; }
    FrameRing& Ring() { return ring_; }

    static const char* StateName(State s);

private:
    void WorkerMain(uint32_t id, CameraFormat fmt);
    void Fail(State s, const std::string& why);
    // Producer side, under publishMutex_ (FrameRing is SINGLE-producer; the future MJPG
    // pool publishes from N threads).
    void PublishNV12(const uint8_t* pixels, int pitch, int w, int h, bool fullRange);
    friend class MjpegPool;
    void PublishI420(const uint8_t* const plane[3], const int linesize[3], int w, int h,
                     bool fullRange);
    void NotePublished();

    FrameRing ring_;
    std::mutex publishMutex_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> removed_{false};
    std::atomic<State> state_{State::Closed};
    std::atomic<int> w_{0}, h_{0};
    int fpsReq_ = 0;
    uint32_t id_ = 0;
    std::string name_;
    mutable std::mutex statsMutex_;
    Stats stats_;
    std::string lastError_;
    // 1 s delivered-fps window (worker-owned).
    double fpsWindowStart_ = 0.0;
    uint64_t fpsWindowCount_ = 0;
    MjpegPool* pool_ = nullptr;      // owned; only for MJPG devices (#90 follow-up)
    int64_t lastPublishedSeq_ = -1;  // MJPG in-order gate (under publishMutex_)
};

}  // namespace mp
