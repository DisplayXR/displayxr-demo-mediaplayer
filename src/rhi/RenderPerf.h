// SPDX-License-Identifier: Apache-2.0
//
// RenderPerf - the render thread's own timings, always on: a few clock reads per frame, no
// allocation, no lock (everything is on the render thread). VulkanRenderer feeds a
// RenderPerfMeter; every kWindowSec it closes a window, which Media Player Pro logs while a
// live source is up and puts in get_rig_status / station_status.json ("render"). It is the
// on-site proof of where a frame's time goes: a station that can only run signed release
// builds has no profiler. Pure (no Vulkan): tests/render_perf_test.cpp.
//
// The window also splits the render loop into stages (RenderStage), which App times around
// each step of one loop iteration. Per frame the stages add up to the busy time, so a slow
// frame names its stage.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace mp {

// The stages of one render-loop iteration (App::Run's loop body: the event pump, then
// RenderOneFrame), in the order they run. Per frame they add up to the loop's period minus
// xrWaitFrame, which is the busy time. Other is the remainder no stage covers: the UI
// fades, mode / convert / depth-cursor upkeep, the station's alert log, the fps counter.
// The Pro-only stages read 0 in the base player.
enum class RenderStage : int {
    Pump = 0,  // window events + keys, xrPollEvent, file picks, PollLive / PollStream
    Upload,    // the source frame: ring acquire, live layout, the plane upload (incl. the staging copy)
    Rig,       // Pro: the rig check's Submit (its Downsample on the frames it measures)
    Thumbs,    // Pro: the live input's thumbnail (twice a second)
    Scopes,    // Pro: ProController::OnFrame (the scopes / pixel-probe gather while they are on)
    AutoConv,  // auto-convergence: sampling (~5 Hz while on) + the smoothed shift
    ProTick,   // Pro: ProController::Tick (station, looks, Resolve, the `perf:` line)
    UiBuild,   // ImGui new frame + building the UI (transport, Pro panels)
    UiRender,  // ImGui render into the HUD image, recorded into the frame's command buffer
    Draw,      // the picture: view geometry + recording DrawViews / ClearViews
    Submit,    // the frame's one vkQueueSubmit
    XrBegin,   // xrBeginFrame + xrLocateViews + the projection swapchain acquire / wait
    HudImage,  // the HUD swapchain image: acquire / wait + release
    XrEnd,     // xrEndFrame (the runtime's in-process compositor and weaver)
    Other,     // busy minus the stages above
    Count
};
constexpr int kRenderStages = (int)RenderStage::Count;

// A stage's name in the `perf:` line and its key under rig.render.stages.
inline const char* RenderStageName(RenderStage s) {
    switch (s) {
        case RenderStage::Pump: return "pump";
        case RenderStage::Upload: return "upload";
        case RenderStage::Rig: return "rig";
        case RenderStage::Thumbs: return "thumbs";
        case RenderStage::Scopes: return "scopes";
        case RenderStage::AutoConv: return "autoconv";
        case RenderStage::ProTick: return "pro_tick";
        case RenderStage::UiBuild: return "ui_build";
        case RenderStage::UiRender: return "ui_render";
        case RenderStage::Draw: return "draw";
        case RenderStage::Submit: return "submit";
        case RenderStage::XrBegin: return "xr_begin";
        case RenderStage::HudImage: return "hud_image";
        case RenderStage::XrEnd: return "xr_end";
        case RenderStage::Other: return "other";
        default: return "?";
    }
}

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
    // Of that, blocked in xrWaitFrame (the display's pacing; XrSession times the call alone,
    // the rest of its BeginFrame is the xr_begin stage), and the rest: the render thread's
    // own time per frame (avg frame - avg wait). A render thread that keeps up has busy well
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
    // The render loop's stages (RenderStage): the average per loop iteration over the
    // window, and the worst single iteration. A stage that runs twice a second is averaged
    // over every frame, so the averages add up: their sum + xrWaitAvgMs is the loop's
    // period, i.e. the sum is busyAvgMs.
    struct StageStat {
        double avgMs = 0.0, maxMs = 0.0;
    };
    StageStat stages[kRenderStages];
    uint32_t stageFrames = 0;  // loop iterations the stages average over
    double StagesSumMs() const {
        double sum = 0.0;
        for (const StageStat& st : stages) sum += st.avgMs;
        return sum;
    }
};

// The stages for the `perf:` line, in loop order, avg / max ms per frame:
// "sum 19.97 = pump 0.10/0.52, upload 9.10/12.40, ..., other 0.31/1.20".
inline std::string FormatRenderStages(const RenderPerfWindow& w) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "sum %.2f =", w.StagesSumMs());
    std::string out = buf;
    for (int i = 0; i < kRenderStages; ++i) {
        std::snprintf(buf, sizeof(buf), "%s %s %.2f/%.2f", i ? "," : "", RenderStageName((RenderStage)i),
                      w.stages[i].avgMs, w.stages[i].maxMs);
        out += buf;
    }
    return out;
}

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
    void XrWait(double ms) {
        xrWait_.Add(ms);
        loopWait_ += ms;
    }
    void FrameOpen(double ms) { frameOpen_.Add(ms); }
    void Upload(double ms, double copyMs, double bytes, int copyThreads) {
        upload_.Add(ms);
        copy_.Add(copyMs);
        uploadBytes_ += bytes;
        copyThreads_ = copyThreads;
    }
    void CountStagingAlloc() { ++stagingAllocs_; }
    void CountStagingOneOff() { ++stagingOneOff_; }

    // The render loop's stages. Stage() adds to the open loop iteration (several calls for
    // one stage add up; Other is computed, never reported). EndLoop() closes the iteration
    // with its wall time, loop top to loop top: what neither a stage nor xrWaitFrame covered
    // is Other. An iteration counts in the window that is open when it ends.
    void Stage(RenderStage s, double ms) {
        const int i = (int)s;
        if (i >= 0 && i < (int)RenderStage::Other && ms > 0.0) loopStage_[i] += ms;
    }
    void EndLoop(double loopMs) {
        const int other = (int)RenderStage::Other;
        double covered = loopWait_;
        for (int i = 0; i < other; ++i) covered += loopStage_[i];
        loopStage_[other] = loopMs > covered ? loopMs - covered : 0.0;
        for (int i = 0; i < kRenderStages; ++i) {
            stageSum_[i] += loopStage_[i];
            if (loopStage_[i] > stageMax_[i]) stageMax_[i] = loopStage_[i];
            loopStage_[i] = 0.0;
        }
        loopWait_ = 0.0;
        ++loops_;
    }

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
        w.stageFrames = loops_;
        for (int i = 0; i < kRenderStages; ++i) {
            w.stages[i].avgMs = loops_ ? stageSum_[i] / loops_ : 0.0;
            w.stages[i].maxMs = stageMax_[i];
            stageSum_[i] = stageMax_[i] = 0.0;
        }
        last_ = w;
        windowStart_ = nowSec;
        frames_ = 0;
        frame_ = xrWait_ = frameOpen_ = upload_ = copy_ = PerfAcc{};
        uploadBytes_ = 0.0;
        loops_ = 0;  // the open iteration (loopStage_, loopWait_) carries into the new window
    }

    double windowSec_;
    double windowStart_ = -1.0, lastSubmit_ = -1.0;
    uint32_t frames_ = 0;
    PerfAcc frame_, xrWait_, frameOpen_, upload_, copy_;
    double uploadBytes_ = 0.0;
    int copyThreads_ = 1;
    uint64_t stagingAllocs_ = 0, stagingOneOff_ = 0;
    double loopStage_[kRenderStages] = {};  // the open loop iteration's stages
    double loopWait_ = 0.0;                 // ... and its xrWaitFrame
    double stageSum_[kRenderStages] = {}, stageMax_[kRenderStages] = {};
    uint32_t loops_ = 0;                    // iterations ended in this window
    RenderPerfWindow last_;
};

}  // namespace mp
