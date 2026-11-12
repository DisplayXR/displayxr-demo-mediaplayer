// SPDX-License-Identifier: Apache-2.0
//
// RenderPerf - the render thread's own timings, always on: a few clock reads per frame, no
// allocation, no lock (everything is on the render thread). VulkanRenderer feeds a
// RenderPerfMeter; every kWindowSec it closes a window, which Media Player Pro logs while a
// live source is up and puts in get_rig_status / station_status.json ("render"). It is the
// on-site proof of where a frame's time goes: a station that can only run signed release
// builds has no profiler. Pure (no Vulkan): tests/render_perf_test.cpp.
#pragma once

#include <cstdint>

namespace mp {

// One timing over a window: count, mean and worst, in ms.
struct PerfAcc {
    uint32_t n = 0;
    double sum = 0.0, max = 0.0;
    void Add(double v) {
        ++n;
        sum += v;
        if (v > max) max = v;
    }
    double Avg() const { return n ? sum / n : 0.0; }
};

// A closed window of the render thread's timings.
struct RenderPerfWindow {
    uint64_t serial = 0;        // 0 = no window has closed yet; +1 per window
    double seconds = 0.0;       // the window's length
    uint32_t frames = 0;        // frames submitted in it
    double fps = 0.0;           // frames / seconds
    // Submit-to-submit interval: the render loop's period (16.7 ms at 60 Hz).
    double frameAvgMs = 0.0, frameMaxMs = 0.0;
    // Of that, blocked in xrWaitFrame (the display's pacing; App times its BeginFrame, which
    // is xrWaitFrame + xrBeginFrame + xrLocateViews), and the rest: the render thread's own
    // time per frame (avg frame - avg wait). A render thread that keeps up has busy well
    // under the display period and spends the rest waiting.
    double xrWaitAvgMs = 0.0;
    double busyAvgMs = 0.0;
    // Opening the frame's command buffer: the wait for the ring slot's fence (the GPU N
    // frames behind) plus that slot's deferred releases.
    double frameOpenAvgMs = 0.0, frameOpenMaxMs = 0.0;
    // Source uploads (a video / live frame, a still, the compare clip): recording plus the
    // CPU copy into staging, which is most of it; average MB per upload.
    uint32_t uploads = 0;
    double uploadAvgMs = 0.0, uploadMaxMs = 0.0;
    double copyAvgMs = 0.0, copyMaxMs = 0.0;
    double uploadMB = 0.0;
    int copyThreads = 1;        // threads of the last upload's copy (its bands)
    // Since start: persistent staging buffers (re)allocated - a handful at a size change,
    // flat while streaming - and uploads that used a one-off buffer (the same plane twice
    // in one frame).
    uint64_t stagingAllocs = 0, stagingOneOff = 0;
};

class RenderPerfMeter {
public:
    static constexpr double kWindowSec = 10.0;
    explicit RenderPerfMeter(double windowSec = kWindowSec) : windowSec_(windowSec > 0.0 ? windowSec : kWindowSec) {}

    // Once per submitted frame, `nowSec` on a steady clock. Closes the window when it is
    // due; returns true when it did.
    bool FrameSubmitted(double nowSec) {
        if (lastSubmit_ >= 0.0) frame_.Add((nowSec - lastSubmit_) * 1000.0);
        lastSubmit_ = nowSec;
        if (windowStart_ < 0.0) {
            windowStart_ = nowSec;
            return false;
        }
        ++frames_;
        if (nowSec - windowStart_ < windowSec_) return false;
        Close(nowSec);
        return true;
    }
    void XrWait(double ms) { xrWait_.Add(ms); }
    void FrameOpen(double ms) { frameOpen_.Add(ms); }
    void Upload(double ms, double copyMs, double bytes, int copyThreads) {
        upload_.Add(ms);
        copy_.Add(copyMs);
        uploadBytes_ += bytes;
        copyThreads_ = copyThreads;
    }
    void CountStagingAlloc() { ++stagingAllocs_; }
    void CountStagingOneOff() { ++stagingOneOff_; }

    const RenderPerfWindow& Last() const { return last_; }

private:
    void Close(double nowSec) {
        RenderPerfWindow w;
        w.serial = last_.serial + 1;
        w.seconds = nowSec - windowStart_;
        w.frames = frames_;
        w.fps = w.seconds > 0.0 ? frames_ / w.seconds : 0.0;
        w.frameAvgMs = frame_.Avg();
        w.frameMaxMs = frame_.max;
        w.xrWaitAvgMs = xrWait_.Avg();
        w.busyAvgMs = w.frameAvgMs > w.xrWaitAvgMs ? w.frameAvgMs - w.xrWaitAvgMs : 0.0;
        w.frameOpenAvgMs = frameOpen_.Avg();
        w.frameOpenMaxMs = frameOpen_.max;
        w.uploads = upload_.n;
        w.uploadAvgMs = upload_.Avg();
        w.uploadMaxMs = upload_.max;
        w.copyAvgMs = copy_.Avg();
        w.copyMaxMs = copy_.max;
        w.uploadMB = upload_.n ? uploadBytes_ / upload_.n / (1024.0 * 1024.0) : 0.0;
        w.copyThreads = copyThreads_;
        w.stagingAllocs = stagingAllocs_;
        w.stagingOneOff = stagingOneOff_;
        last_ = w;
        windowStart_ = nowSec;
        frames_ = 0;
        frame_ = xrWait_ = frameOpen_ = upload_ = copy_ = PerfAcc{};
        uploadBytes_ = 0.0;
    }

    double windowSec_;
    double windowStart_ = -1.0, lastSubmit_ = -1.0;
    uint32_t frames_ = 0;
    PerfAcc frame_, xrWait_, frameOpen_, upload_, copy_;
    double uploadBytes_ = 0.0;
    int copyThreads_ = 1;
    uint64_t stagingAllocs_ = 0, stagingOneOff_ = 0;
    RenderPerfWindow last_;
};

}  // namespace mp
