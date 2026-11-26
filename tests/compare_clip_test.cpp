// SPDX-License-Identifier: Apache-2.0
//
// pro::DecideSync: the clip wipe's B follows A - mirrors A's pause, pins B to A's frame
// while paused, and while playing (B paced by A's clock) seeks only on a jump: ahead over
// two frames, behind over a second, never past B's end (#250). Maps between the two files'
// first timestamps. pro::HoldStats: B's hold counter.

#include "pro/CompareClip.h"

#include <cmath>
#include <cstdio>

using namespace mp::pro;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

int main() {
    const double fps = 24.0, f = 1.0 / fps;
    // Paused: pause mirrored; within half a frame no seek; further, a seek to A's moment.
    SyncAction a = DecideSync(10.0, 0.0, true, 10.0 + 0.4 * f, 0.0, fps, 100.0, 99.9);
    CHECK(a.pause && a.seekTo < 0.0, "paused, within half a frame: no seek");
    a = DecideSync(10.0, 0.0, true, 10.0 + 0.6 * f, 0.0, fps, 100.0, 99.9);
    CHECK(a.pause && Near(a.seekTo, 10.0), "paused, over half a frame: pinned to A's frame");
    // Playing (#250): B is paced by A's clock, so drift is left to the pacing; only a jump
    // seeks, and not twice within 0.5 s.
    a = DecideSync(10.0, 0.0, false, 10.0 + 1.5 * f, 0.0, fps, 100.0, 0.0);
    CHECK(!a.pause && a.seekTo < 0.0, "playing, 1.5 frames ahead: left alone");
    a = DecideSync(10.0, 0.0, false, 10.0 - 3.0 * f, 0.0, fps, 100.0, 0.0);
    CHECK(a.seekTo < 0.0, "playing, 3 frames behind: B catches up by decoding, no seek");
    a = DecideSync(10.0, 0.0, false, 10.0 + 3.0 * f, 0.0, fps, 100.0, 0.0);
    CHECK(Near(a.seekTo, 10.0), "playing, 3 frames ahead (A seeked back): seek");
    a = DecideSync(10.0, 0.0, false, 10.0 + 3.0 * f, 0.0, fps, 100.0, 99.8);
    CHECK(a.seekTo < 0.0, "playing, a seek 0.2 s ago: hold off");
    a = DecideSync(10.0, 0.0, false, 8.0, 0.0, fps, 100.0, 0.0);
    CHECK(Near(a.seekTo, 10.0), "playing, 2 s behind (A seeked forward): seek");
    // Loops: B does not loop itself; A's wrap sends it back, B's early tail is not a jump,
    // and a B shorter than A holds its last frame instead of being chased past its end.
    a = DecideSync(0.042, 0.0, false, 24.6, 0.0, fps, 100.0, 0.0, 24.7);
    CHECK(Near(a.seekTo, 0.042), "A wrapped while B sits at its end: B back to A's start");
    a = DecideSync(24.6, 0.0, false, 24.6 + 3.0 * f, 0.0, fps, 100.0, 0.0, 24.7);
    CHECK(a.seekTo < 0.0, "B's last frames drain 3 frames early: no seek");
    a = DecideSync(30.0, 0.0, false, 24.66, 0.0, fps, 100.0, 0.0, 24.7);
    CHECK(a.seekTo < 0.0, "playing, A past B's end: B holds, no seek");
    a = DecideSync(30.0, 0.0, true, 24.66, 0.0, fps, 100.0, 0.0, 24.7);
    CHECK(a.seekTo < 0.0, "paused, A past B's end: B holds, no seek");
    // Different first timestamps (an MXF that starts at 3600 s): the target is B's own.
    a = DecideSync(3610.0, 3600.0, true, 0.0, 0.0, fps, 100.0, 0.0);
    CHECK(Near(a.seekTo, 10.0), "A starts at 3600 s, B at 0: B seeks to 10 s");
    a = DecideSync(10.0, 0.0, true, 3610.0, 3600.0, fps, 100.0, 0.0);
    CHECK(a.seekTo < 0.0, "B starts at 3600 s and is at A's moment: no seek");
    // Unknown frame rate: 24 assumed.
    a = DecideSync(10.0, 0.0, true, 10.0 + 0.6 / 24.0, 0.0, 0.0, 100.0, 0.0);
    CHECK(Near(a.seekTo, 10.0), "unknown fps: half a 24p frame");
    // Hold counter: a gap over 250 ms is a hold; a Break (pause, A's jump) is not.
    HoldStats h;
    h.Frame(0.0);
    h.Frame(1.0 / 24.0);
    CHECK(h.Frame(0.4), "a 0.36 s gap is a hold");
    h.Break();
    CHECK(!h.Frame(2.0), "the first frame after a break is not a hold");
    CHECK(h.holds == 1 && std::fabs(h.longest - (0.4 - 1.0 / 24.0)) < 1e-9, "one hold, longest 0.36 s");
    if (g_failures) {
        std::fprintf(stderr, "compare_clip_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("compare_clip_test: all passed\n");
    return 0;
}
