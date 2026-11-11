// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the media browser's TouchScroller (#160): tap vs drag, the drag follows
// the finger past the slop, a fling travels speed x tau and stops at the ends, a finger
// that stopped before lifting does not fling, a press that stops a fling is no tap
// unless the fling was nearly done, and a press held still is a long press, not a tap.

#include "browser/TouchScroller.h"

#include <cmath>
#include <cstdio>

using mp::browser::TouchScroller;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

constexpr float kSlop = 10.0f;
constexpr double kFrame = 1.0 / 60.0;
constexpr double kTau = 0.4;  // TouchScroller::kFlingTau

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

TouchScroller Make(float limit) {
    TouchScroller s;
    s.SetSlop(kSlop);
    s.SetLimit(limit);
    return s;
}

// A drag upwards at `speed` px/s for `frames` frames from y = 1000, starting at t = 0.
// Returns the time of the last move.
double DragUp(TouchScroller& s, float speed, int frames) {
    s.Press(100.0f, 1000.0f, 0.0);
    double t = 0.0;
    for (int i = 1; i <= frames; ++i) {
        t = i * kFrame;
        s.Move(100.0f, 1000.0f - speed * (float)t, t);
    }
    return t;
}

void TestTap() {
    TouchScroller s = Make(1000.0f);
    s.Press(100.0f, 100.0f, 0.0);
    s.Move(104.0f, 106.0f, 0.05);
    CHECK(!s.Dragging(), "a move within the slop is no drag");
    CHECK(s.Release(0.1), "and the press is a tap");
    CHECK(s.Offset() == 0.0f, "a tap does not scroll");

    s.Press(100.0f, 100.0f, 1.0);
    s.Move(130.0f, 100.0f, 1.05);
    CHECK(!s.Dragging(), "a sideways move does not scroll");
    CHECK(!s.Release(1.1), "but it is no tap either");
    CHECK(!s.Release(1.2), "a release without a press is no tap");
}

void TestDrag() {
    TouchScroller s = Make(1000.0f);
    s.Press(0.0f, 500.0f, 0.0);
    s.Move(0.0f, 400.0f, 0.1);
    CHECK(s.Dragging(), "past the slop is a drag");
    CHECK(Near(s.Offset(), 100.0f - kSlop, 0.01f), "the content follows the finger, less the slop");
    s.Move(0.0f, 300.0f, 0.2);
    CHECK(Near(s.Offset(), 200.0f - kSlop, 0.01f), "and keeps following it");
    s.Move(0.0f, -2000.0f, 0.3);
    CHECK(s.Offset() == 1000.0f, "stops at the end");
    s.Move(0.0f, -1990.0f, 0.4);
    CHECK(Near(s.Offset(), 990.0f, 0.01f), "and turns back at once");
    s.Move(0.0f, -1990.0f, 1.0);
    CHECK(!s.Release(1.0), "a drag is no tap");
    s.Update(5.0);
    CHECK(Near(s.Offset(), 990.0f, 0.01f), "a finger that stopped before lifting does not fling");
}

void TestFling() {
    TouchScroller s = Make(100000.0f);
    const double t = DragUp(s, 1200.0f, 12);
    const float atRelease = s.Offset();
    s.Release(t);
    s.Update(t + 0.2);
    CHECK(s.Offset() > atRelease, "a fling keeps moving after the release");
    s.Update(t + 10.0);
    CHECK(Near(s.Offset(), atRelease + 1200.0f * (float)kTau, 2.0f), "and travels speed x tau");

    TouchScroller ends = Make(300.0f);
    const double t2 = DragUp(ends, 3000.0f, 6);
    ends.Release(t2);
    ends.Update(t2 + 10.0);
    CHECK(ends.Offset() == 300.0f, "a fling stops at the end");

    s.SetLimit(50.0f);
    CHECK(s.Offset() == 50.0f, "a smaller limit clamps the offset");
}

void TestCatch() {
    TouchScroller s = Make(100000.0f);
    const double t = DragUp(s, 1200.0f, 12);
    s.Release(t);
    s.Update(t + 0.1);
    const float caught = s.Offset();
    s.Press(100.0f, 500.0f, t + 0.1);
    s.Update(t + 1.0);
    CHECK(s.Offset() == caught, "a press stops a fling");
    CHECK(!s.Release(t + 1.1), "and is no tap");
    s.Press(100.0f, 500.0f, t + 2.0);
    CHECK(s.Release(t + 2.1), "the next press is a tap again");

    TouchScroller tail = Make(100000.0f);
    const double t2 = DragUp(tail, 1200.0f, 12);
    tail.Release(t2);
    tail.Update(t2 + 1.7);  // 480 px in all, about 7 px left: still moving
    tail.Press(100.0f, 500.0f, t2 + 1.7);
    CHECK(tail.Release(t2 + 1.8), "a press on a fling with less than the slop to go is a tap");
}

void TestLongPress() {
    TouchScroller s = Make(1000.0f);
    s.Press(100.0f, 100.0f, 0.0);
    s.Move(104.0f, 106.0f, 0.2);
    CHECK(!s.TakeLongPress(0.4), "0.4 s held is no long press yet");
    CHECK(s.TakeLongPress(0.5), "0.5 s held within the slop is a long press");
    CHECK(!s.TakeLongPress(0.6), "reported once");
    CHECK(!s.Release(0.7), "and its release is no tap");

    s.Press(100.0f, 100.0f, 1.0);
    s.Move(100.0f, 130.0f, 1.1);
    CHECK(!s.TakeLongPress(1.6), "a press that left the slop is no long press");
    s.Release(1.7);

    s.Press(100.0f, 100.0f, 2.0);
    CHECK(s.Release(2.1), "a short press is still a tap");
    CHECK(!s.TakeLongPress(3.0), "nor a long press once released");
}

}  // namespace

int main() {
    TestTap();
    TestDrag();
    TestFling();
    TestCatch();
    TestLongPress();
    if (g_failures) {
        std::fprintf(stderr, "touch_scroller_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("touch_scroller_test: all passed\n");
    return 0;
}
