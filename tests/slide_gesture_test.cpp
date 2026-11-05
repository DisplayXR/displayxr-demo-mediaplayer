// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SlideGesture: a tap does nothing, the picture follows a horizontal drag
// and not a vertical one, a swipe slides out and asks for the file once, a short drag and
// a cancel slide back, the picture gives a little with no neighbour that way, nothing
// starts while an open is pending, and a hurried slide ends at once.

#include "browser/SlideGesture.h"

#include <cmath>
#include <cstdio>

using mp::browser::SlideGesture;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

constexpr float kWidth = 1000.0f;
constexpr double kDone = 1.0;  // well past the slide

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }

SlideGesture Make(bool previous, bool next) {
    SlideGesture s;
    s.SetMetrics(kWidth, 10.0f, 100.0f);
    s.SetNeighbours(previous, next);
    return s;
}

// Press at (500, 500), move by (dx, dy), release, all at t = 0.
void Drag(SlideGesture& s, float dx, float dy) {
    s.Press(500.0f, 500.0f);
    s.Move(500.0f + dx, 500.0f + dy);
    s.Release(0.0);
}

void TestTap() {
    SlideGesture s = Make(true, true);
    Drag(s, 4.0f, 0.0f);
    CHECK(s.Offset() == 0.0f, "a tap does not move the picture");
    CHECK(s.Update(kDone) == 0, "a tap opens nothing");
}

void TestFollow() {
    SlideGesture s = Make(true, true);
    s.Press(500.0f, 500.0f);
    s.Move(300.0f, 520.0f);
    CHECK(Near(s.Offset(), -0.2f), "the picture follows the finger");
    CHECK(s.Neighbour() == 1, "dragging left shows the next file");
    s.Move(700.0f, 520.0f);
    CHECK(Near(s.Offset(), 0.2f), "and back the other way");
    CHECK(s.Neighbour() == -1, "dragging right shows the previous file");

    SlideGesture v = Make(true, true);
    v.Press(500.0f, 500.0f);
    v.Move(520.0f, 800.0f);
    CHECK(v.Offset() == 0.0f, "a vertical drag does not move the picture");
}

void TestSwipe() {
    SlideGesture s = Make(true, true);
    Drag(s, -200.0f, 0.0f);
    CHECK(s.Update(0.1) == 0, "nothing opens mid-slide");
    CHECK(s.Offset() < -0.2f && s.Offset() > -1.0f, "the picture slides on");
    CHECK(s.Update(kDone) == 1, "the slide out asks for the next file");
    CHECK(Near(s.Offset(), -1.0f), "the picture is out");
    CHECK(s.Neighbour() == 1 && s.Waiting(), "the next file's page stays while it opens");
    CHECK(s.Update(kDone + 1.0) == 0, "and asks only once");
    s.Press(500.0f, 500.0f);
    s.Move(200.0f, 500.0f);
    CHECK(Near(s.Offset(), -1.0f), "no new drag while the file opens");
    s.Reset();
    CHECK(s.Offset() == 0.0f && s.Neighbour() == 0 && !s.Waiting(), "reset is rest");

    SlideGesture p = Make(true, true);
    Drag(p, 200.0f, 0.0f);
    CHECK(p.Update(kDone) == -1, "a swipe right asks for the previous file");
    CHECK(Near(p.Offset(), 1.0f), "the picture leaves on the right");

    SlideGesture d = Make(true, true);
    Drag(d, -200.0f, -150.0f);
    CHECK(d.Update(kDone) == 0, "a diagonal release slides back");

    SlideGesture h = Make(true, true);
    Drag(h, -200.0f, 0.0f);
    CHECK(h.Busy(), "a slide is busy");
    h.Hurry();
    CHECK(h.Update(0.01) == 1, "a hurried slide ends at the next update");
    CHECK(h.Busy() && h.Waiting(), "and waits for the file");
}

void TestBack() {
    SlideGesture s = Make(true, true);
    Drag(s, -50.0f, 0.0f);
    CHECK(s.Update(kDone) == 0, "a short drag opens nothing");
    CHECK(s.Offset() == 0.0f && !s.Waiting(), "and slides back");

    SlideGesture c = Make(true, true);
    c.Press(500.0f, 500.0f);
    c.Move(200.0f, 500.0f);
    c.Cancel(0.0);
    c.Release(0.0);
    CHECK(c.Update(kDone) == 0 && c.Offset() == 0.0f, "a cancelled drag slides back");
}

void TestGive() {
    SlideGesture s = Make(true, false);
    s.Press(500.0f, 500.0f);
    s.Move(100.0f, 500.0f);
    CHECK(Near(s.Offset(), -0.1f), "with no next file the picture gives a quarter");
    CHECK(s.Neighbour() == 0, "and no page comes in");
    s.Release(0.0);
    CHECK(s.Update(kDone) == 0 && s.Offset() == 0.0f, "then slides back");
}

}  // namespace

int main() {
    TestTap();
    TestFollow();
    TestSwipe();
    TestBack();
    TestGive();
    if (g_failures) {
        std::fprintf(stderr, "slide_gesture_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("slide_gesture_test: all passed\n");
    return 0;
}
