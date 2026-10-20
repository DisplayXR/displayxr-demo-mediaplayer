// SPDX-License-Identifier: Apache-2.0
//
// pro::DecideSync: the clip wipe's B follows A - mirrors A's pause, pins B to A's frame
// while paused, corrects a playing drift over two frames at most every 0.5 s, and maps
// between the two files' first timestamps.

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
    // Playing: small drift tolerated, large drift corrected, but not twice within 0.5 s.
    a = DecideSync(10.0, 0.0, false, 10.0 + 1.5 * f, 0.0, fps, 100.0, 0.0);
    CHECK(!a.pause && a.seekTo < 0.0, "playing, 1.5 frames off: left alone");
    a = DecideSync(10.0, 0.0, false, 10.0 - 3.0 * f, 0.0, fps, 100.0, 0.0);
    CHECK(Near(a.seekTo, 10.0), "playing, 3 frames behind: seek");
    a = DecideSync(10.0, 0.0, false, 10.0 - 3.0 * f, 0.0, fps, 100.0, 99.8);
    CHECK(a.seekTo < 0.0, "playing, a seek 0.2 s ago: hold off");
    // Different first timestamps (an MXF that starts at 3600 s): the target is B's own.
    a = DecideSync(3610.0, 3600.0, true, 0.0, 0.0, fps, 100.0, 0.0);
    CHECK(Near(a.seekTo, 10.0), "A starts at 3600 s, B at 0: B seeks to 10 s");
    a = DecideSync(10.0, 0.0, true, 3610.0, 3600.0, fps, 100.0, 0.0);
    CHECK(a.seekTo < 0.0, "B starts at 3600 s and is at A's moment: no seek");
    // Unknown frame rate: 24 assumed.
    a = DecideSync(10.0, 0.0, true, 10.0 + 0.6 / 24.0, 0.0, 0.0, 100.0, 0.0);
    CHECK(Near(a.seekTo, 10.0), "unknown fps: half a 24p frame");
    if (g_failures) {
        std::fprintf(stderr, "compare_clip_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("compare_clip_test: all passed\n");
    return 0;
}
