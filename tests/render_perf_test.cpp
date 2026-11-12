// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the render thread's perf window (rhi/RenderPerf.h), the numbers Media
// Player Pro logs (`perf:`) and reports (get_rig_status / station_status.json "render"):
// no window before one has run its length; frame period, fps, xrWaitFrame and the busy
// time derived from them; upload / copy averages, worst and MB; a spike shows as the max;
// each window starts clean while the staging counters run on. Pure, header-only.

#include "rhi/RenderPerf.h"

#include <cmath>
#include <cstdio>

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

}  // namespace

int main() {
    WindowAtItsLength();
    SpikeAndNextWindow();
    WaitLongerThanFrame();
    if (g_failures) {
        std::fprintf(stderr, "render_perf_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("render_perf_test: all passed\n");
    return 0;
}
