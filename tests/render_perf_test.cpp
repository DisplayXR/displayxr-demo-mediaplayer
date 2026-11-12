// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the render thread's perf window (rhi/RenderPerf.h), the numbers Media
// Player Pro logs (`perf:`) and reports (get_rig_status / station_status.json "render"):
// no window before one has run its length; frame period, fps, xrWaitFrame and the busy
// time derived from them; upload / copy averages, worst and MB; a spike shows as the max;
// each window starts clean while the staging counters run on. The render loop's stages:
// per-frame averages that add up to busy (Other is the remainder, never negative), the worst
// frame per stage, an open loop iteration carried into the next window, and the `perf:`
// text. Media Player Pro's A/B switches (pro/PerfSwitches.h) parse and name what is off.
// Pure, header-only.

#include "pro/PerfSwitches.h"
#include "rhi/RenderPerf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

bool Near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

// A 60 Hz frame clock: frame k at base + k / 60 s (computed, not accumulated, so a 10 s
// window ends exactly on frame 600).
struct Clock60 {
    double base = 0.0;
    int k = 0;
    double Next() { return base + (double)(++k) / 60.0; }
};

// One frame: an xrWaitFrame of waitMs, a frame open, an upload, then the submit.
bool Frame(mp::RenderPerfMeter& m, double now, double waitMs, double uploadMs, double copyMs, double bytes) {
    m.XrWait(waitMs);
    m.FrameOpen(0.05);
    m.Upload(uploadMs, copyMs, bytes, 4);
    return m.FrameSubmitted(now);
}

void WindowAtItsLength() {
    const double mb50 = 50.0 * 1024 * 1024;
    mp::RenderPerfMeter m(10.0);
    CHECK(m.Last().serial == 0, "no window before the first closes");
    Clock60 c{100.0, 0};
    m.FrameSubmitted(c.base);  // the first frame starts the window
    int closed = 0, closedAt = -1;
    for (int i = 0; i < 610; ++i)
        if (Frame(m, c.Next(), 10.0, 3.0, 2.5, mb50)) {
            ++closed;
            if (closedAt < 0) closedAt = c.k;
        }
    const mp::RenderPerfWindow& w = m.Last();
    CHECK(closed == 1 && closedAt == 600 && w.serial == 1, "the window closes at its length (frame 600), once");
    CHECK(w.frames == 600 && Near(w.seconds, 10.0) && Near(w.fps, 60.0), "600 frames over 10 s = 60 fps");
    CHECK(Near(w.frameAvgMs, 1000.0 / 60.0) && Near(w.frameMaxMs, 1000.0 / 60.0), "frame period 16.67 ms");
    CHECK(Near(w.xrWaitAvgMs, 10.0) && Near(w.busyAvgMs, 1000.0 / 60.0 - 10.0), "busy = frame - xrWaitFrame (6.67 ms)");
    CHECK(Near(w.frameOpenAvgMs, 0.05) && Near(w.frameOpenMaxMs, 0.05), "frame open avg / max");
    CHECK(w.uploads == 600 && Near(w.uploadAvgMs, 3.0) && Near(w.uploadMaxMs, 3.0) && Near(w.copyAvgMs, 2.5) &&
              Near(w.copyMaxMs, 2.5) && Near(w.uploadMB, 50.0) && w.copyThreads == 4,
          "uploads: count, avg / max ms, copy ms, MB, threads");
}

void SpikeAndNextWindow() {
    const double mb1 = 1024.0 * 1024.0;
    mp::RenderPerfMeter m(10.0);
    m.FrameSubmitted(0.0);
    // 5 s at 60 Hz, then one 40 ms frame (a stall) with a slow upload, then 60 Hz until
    // the window closes
    Clock60 c{0.0, 0};
    for (int i = 0; i < 300; ++i) Frame(m, c.Next(), 8.0, 2.0, 1.5, mb1);
    m.CountStagingAlloc();
    m.CountStagingAlloc();
    m.CountStagingOneOff();
    c.base += 0.040;  // the stall: every later frame 40 ms later
    m.Upload(9.0, 8.0, mb1, 4);
    m.FrameSubmitted(c.base + c.k / 60.0);
    bool closed = false;
    for (int i = 0; i < 400 && !closed; ++i) closed = Frame(m, c.Next(), 8.0, 2.0, 1.5, mb1);
    const mp::RenderPerfWindow w = m.Last();
    CHECK(closed && w.serial == 1, "one window");
    CHECK(Near(w.frameMaxMs, 40.0), "the 40 ms frame is the window's worst");
    CHECK(Near(w.uploadMaxMs, 9.0) && Near(w.copyMaxMs, 8.0), "the slow upload is the worst upload");
    CHECK(w.frameAvgMs > 1000.0 / 60.0 && w.frameAvgMs < 17.0, "one stall barely moves the average");
    CHECK(w.stagingAllocs == 2 && w.stagingOneOff == 1, "staging counters");
    // a quiet second window: 40 fps, no uploads, no xrWaitFrame reported; the staging
    // counters run on
    m.CountStagingAlloc();
    double t = c.base + c.k / 60.0;
    for (int i = 0; i < 1000 && m.Last().serial == 1; ++i) {
        t += 0.025;
        m.FrameSubmitted(t);
    }
    const mp::RenderPerfWindow w2 = m.Last();
    CHECK(w2.serial == 2, "the next window closes");
    CHECK(w2.uploads == 0 && Near(w2.uploadAvgMs, 0.0) && Near(w2.uploadMB, 0.0) && Near(w2.xrWaitAvgMs, 0.0),
          "a window starts clean: no uploads, no waits carried over");
    CHECK(Near(w2.frameAvgMs, 25.0) && Near(w2.fps, 40.0, 0.2) && Near(w2.busyAvgMs, 25.0),
          "40 fps, nothing reported as xrWaitFrame: all busy");
    CHECK(w2.stagingAllocs == 3 && w2.stagingOneOff == 1, "the staging counters are since start");
}

void WaitLongerThanFrame() {
    // xrWaitFrame can be reported longer than the submit-to-submit average (a frame not
    // submitted): busy is never negative.
    mp::RenderPerfMeter m(1.0);
    m.FrameSubmitted(0.0);
    for (int i = 1; i <= 70; ++i) {
        m.XrWait(30.0);
        m.FrameSubmitted(i / 60.0);
    }
    CHECK(m.Last().serial == 1 && Near(m.Last().busyAvgMs, 0.0), "busy clamps at 0");
}

const mp::RenderPerfWindow::StageStat& St(const mp::RenderPerfWindow& w, mp::RenderStage s) {
    return w.stages[(int)s];
}

void StagesAddUpToBusy() {
    // A 60 Hz render loop: every iteration pumps (0.2 ms), uploads in two calls (8.0 + 0.3),
    // waits 0.5 ms in xrWaitFrame, builds the UI (1.0) and ends the frame (4.0); every 60th
    // iteration the rig check measures (2.0). The rest of the 16.67 ms is Other.
    using mp::RenderStage;
    const double period = 1000.0 / 60.0;
    mp::RenderPerfMeter m(10.0);
    m.FrameSubmitted(0.0);
    Clock60 c{0.0, 0};
    int spikes = 0;
    for (int i = 1; i <= 600; ++i) {
        m.Stage(RenderStage::Pump, 0.2);
        m.Stage(RenderStage::Upload, 8.0);
        m.Stage(RenderStage::Upload, 0.3);
        if (i % 60 == 0) {
            m.Stage(RenderStage::Rig, 2.0);
            if (i < 600) ++spikes;  // iteration 600 ends after the window closes
        }
        m.XrWait(0.5);
        m.Stage(RenderStage::UiBuild, 1.0);
        m.FrameSubmitted(c.Next());  // the 600th closes the window, before its EndLoop
        m.Stage(RenderStage::XrEnd, 4.0);
        m.EndLoop(period);
    }
    const mp::RenderPerfWindow& w = m.Last();
    CHECK(w.serial == 1 && w.stageFrames == 599, "the window holds the 599 iterations that ended in it");
    CHECK(Near(St(w, RenderStage::Pump).avgMs, 0.2) && Near(St(w, RenderStage::Pump).maxMs, 0.2), "pump avg / max");
    CHECK(Near(St(w, RenderStage::Upload).avgMs, 8.3) && Near(St(w, RenderStage::Upload).maxMs, 8.3),
          "two calls in one iteration add up (upload 8.3 ms)");
    CHECK(spikes == 9 && Near(St(w, RenderStage::Rig).avgMs, 2.0 * spikes / 599.0) &&
              Near(St(w, RenderStage::Rig).maxMs, 2.0),
          "a stage that runs now and then: averaged over every frame, its max is one run");
    CHECK(Near(St(w, RenderStage::Draw).avgMs, 0.0) && Near(St(w, RenderStage::Draw).maxMs, 0.0),
          "a stage that never ran reads 0");
    const double otherNormal = period - (0.2 + 8.3 + 0.5 + 1.0 + 4.0);
    CHECK(Near(St(w, RenderStage::Other).maxMs, otherNormal) &&
              Near(St(w, RenderStage::Other).avgMs, otherNormal - 2.0 * spikes / 599.0),
          "Other: the loop's time no stage and no xrWaitFrame covered");
    CHECK(Near(w.xrWaitAvgMs, 0.5) && Near(w.busyAvgMs, period - 0.5), "busy = frame - xrWaitFrame");
    CHECK(Near(w.StagesSumMs(), w.busyAvgMs, 1e-9), "the stages add up to busy");
}

void StagesOtherAndCarry() {
    using mp::RenderStage;
    mp::RenderPerfMeter m(1.0);
    m.FrameSubmitted(0.0);
    // Loop 1: the stages cover more than the loop's wall time (clock granularity): Other
    // clamps at 0. Other is never reported, a negative time is ignored.
    m.Stage(RenderStage::Draw, 5.0);
    m.Stage(RenderStage::Other, 7.0);
    m.Stage(RenderStage::Submit, -2.0);
    m.EndLoop(3.0);
    // Loop 2 opens before the window closes and ends after: it counts in the next window.
    m.Stage(RenderStage::XrEnd, 4.0);
    m.XrWait(1.5);
    m.FrameSubmitted(1.0);
    const mp::RenderPerfWindow w1 = m.Last();
    CHECK(w1.serial == 1 && w1.stageFrames == 1, "window 1: one iteration");
    CHECK(Near(St(w1, RenderStage::Draw).avgMs, 5.0) && Near(St(w1, RenderStage::Other).avgMs, 0.0) &&
              Near(St(w1, RenderStage::Submit).maxMs, 0.0) && Near(St(w1, RenderStage::XrEnd).maxMs, 0.0),
          "Other clamps at 0, reported Other and negative times are ignored, the open iteration is not in");
    m.EndLoop(10.0);
    m.FrameSubmitted(2.0);
    const mp::RenderPerfWindow w2 = m.Last();
    CHECK(w2.serial == 2 && w2.stageFrames == 1, "window 2: the carried iteration");
    CHECK(Near(St(w2, RenderStage::XrEnd).avgMs, 4.0) && Near(St(w2, RenderStage::Other).avgMs, 10.0 - 4.0 - 1.5) &&
              Near(St(w2, RenderStage::Draw).maxMs, 0.0),
          "the carried iteration keeps its stages and xrWaitFrame; the window starts clean");
    // A window with no iteration at all (the loop not rendering) reads 0, not NaN.
    m.FrameSubmitted(3.0);
    CHECK(m.Last().serial == 3 && m.Last().stageFrames == 0 && Near(m.Last().StagesSumMs(), 0.0),
          "no iterations: every stage 0");
}

void StagesText() {
    using mp::RenderStage;
    mp::RenderPerfWindow w;
    w.stages[(int)RenderStage::Upload] = {8.25, 12.5};
    w.stages[(int)RenderStage::Rig] = {0.6, 9.0};
    w.stages[(int)RenderStage::Other] = {0.31, 1.2};
    const std::string s = mp::FormatRenderStages(w);
    const char* want =
        "sum 9.16 = pump 0.00/0.00, upload 8.25/12.50, rig 0.60/9.00, thumbs 0.00/0.00, scopes 0.00/0.00, "
        "autoconv 0.00/0.00, pro_tick 0.00/0.00, ui_build 0.00/0.00, ui_render 0.00/0.00, draw 0.00/0.00, "
        "submit 0.00/0.00, xr_begin 0.00/0.00, hud_image 0.00/0.00, xr_end 0.00/0.00, other 0.31/1.20";
    CHECK(s == want, "the `perf:` stages text: the sum, then every stage in loop order, avg/max ms");
    if (s != want) std::fprintf(stderr, "    got:  %s\n    want: %s\n", s.c_str(), want);
    bool namesOk = true;
    for (int i = 0; i < mp::kRenderStages; ++i) {
        if (std::strcmp(mp::RenderStageName((RenderStage)i), "?") == 0) namesOk = false;
        for (int j = 0; j < i; ++j)
            if (std::strcmp(mp::RenderStageName((RenderStage)i), mp::RenderStageName((RenderStage)j)) == 0)
                namesOk = false;
    }
    CHECK(namesOk, "every stage has its own name (the JSON keys)");
}

void ProPerfSwitches() {
    using mp::pro::PerfSwitchOn;
    CHECK(PerfSwitchOn(nullptr) && PerfSwitchOn("1") && PerfSwitchOn("on") && PerfSwitchOn("yes") && PerfSwitchOn(""),
          "unset, 1, on, yes, empty: on");
    CHECK(!PerfSwitchOn("0") && !PerfSwitchOn("off") && !PerfSwitchOn("OFF") && !PerfSwitchOn("False") &&
              !PerfSwitchOn("no"),
          "0, off, false, no (any case): off");
    int logged = 0;
    const auto onSet = [&logged](const char*, const char*, bool, const char*) { ++logged; };
    const mp::pro::PerfSwitches def =
        mp::pro::ReadPerfSwitches([](const char*) -> const char* { return nullptr; }, onSet);
    CHECK(def.rig && def.thumbs && def.autoConv && def.ui && def.Off().empty() && def.OffNames().empty() &&
              logged == 0,
          "nothing set: all on, nothing logged");
    const mp::pro::PerfSwitches s = mp::pro::ReadPerfSwitches(
        [](const char* n) -> const char* {
            if (std::strcmp(n, "MEDIAPLAYER_PRO_RIG") == 0) return "0";
            if (std::strcmp(n, "MEDIAPLAYER_PRO_UI") == 0) return "1";
            if (std::strcmp(n, "MEDIAPLAYER_PRO_AUTOCONV") == 0) return "off";
            return nullptr;
        },
        onSet);
    CHECK(!s.rig && s.thumbs && !s.autoConv && s.ui && logged == 3, "each set variable is read and logged once");
    CHECK(s.Off() == "rig, autoconv" && s.OffNames().size() == 2, "the switches that are off, by name");
    mp::pro::PerfSwitches all;
    all.rig = all.thumbs = all.autoConv = all.ui = false;
    CHECK(all.Off() == "rig, thumbs, autoconv, ui", "all off");
}

}  // namespace

int main() {
    WindowAtItsLength();
    SpikeAndNextWindow();
    WaitLongerThanFrame();
    StagesAddUpToBusy();
    StagesOtherAndCarry();
    StagesText();
    ProPerfSwitches();
    if (g_failures) {
        std::fprintf(stderr, "render_perf_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("render_perf_test: all passed\n");
    return 0;
}
