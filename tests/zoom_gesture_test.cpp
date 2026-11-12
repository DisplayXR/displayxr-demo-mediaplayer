// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for ZoomGesture: a double tap toggles fit and 2x round the tapped point, a
// pinch zooms round its midpoint within fit..4x, one finger pans only while zoomed and never
// past the picture's edges, a slow or moved tap is not a double tap, and Reset() returns to
// fit.

#include "ui/ZoomGesture.h"

#include <cmath>
#include <cstdio>

using mp::ui::ZoomGesture;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

// A 1000 x 2000 window showing a 1000 x 500 picture at fit (a landscape picture, portrait).
ZoomGesture Make() {
    ZoomGesture z;
    z.SetView(1000.0f, 2000.0f, 1000.0f, 500.0f);
    z.SetSlop(10.0f);
    return z;
}

void Tap(ZoomGesture& z, float x, float y, double t) {
    z.Touch(0, x, y, 1, 0.0f, 0.0f, t);
    z.Touch(1, x + 2.0f, y + 1.0f, 1, 0.0f, 0.0f, t + 0.08);
}

void TestDoubleTap() {
    std::printf("[a double tap toggles fit and 2x]\n");
    ZoomGesture z = Make();
    CHECK(!z.Zoomed() && Near(z.Zoom(), 1.0f), "fit at start");
    Tap(z, 500.0f, 1000.0f, 0.0);
    CHECK(!z.Zoomed(), "one tap does nothing");
    Tap(z, 502.0f, 1001.0f, 0.2);
    CHECK(Near(z.Zoom(), 2.0f), "a double tap zooms 2x");
    Tap(z, 500.0f, 1000.0f, 1.0);
    Tap(z, 500.0f, 1000.0f, 1.2);
    CHECK(!z.Zoomed() && Near(z.OffsetX(), 0.0f) && Near(z.OffsetY(), 0.0f), "a second one goes back to fit");
}

void TestDoubleTapPoint() {
    std::printf("[a double tap zooms round the tapped point, within the edges]\n");
    ZoomGesture z = Make();
    // 2x: the picture is 2000 x 1000, so it can move 500 sideways and not at all up or down.
    z.DoubleTap(700.0f, 1000.0f);  // 200 right of the centre
    CHECK(Near(z.OffsetX(), -200.0f), "the tapped point stays under the finger");
    CHECK(Near(z.OffsetY(), 0.0f), "no vertical room: centred");
    z.DoubleTap(0.0f, 0.0f);
    CHECK(!z.Zoomed(), "toggled back");
    z.DoubleTap(1000.0f, 1000.0f);  // the right edge: would need -500, which is the limit
    CHECK(Near(z.OffsetX(), -500.0f), "clamped to the picture's edge");
}

void TestNotDouble() {
    std::printf("[slow, far or moved taps are not a double tap]\n");
    ZoomGesture z = Make();
    Tap(z, 500.0f, 1000.0f, 0.0);
    Tap(z, 500.0f, 1000.0f, 0.6);
    CHECK(!z.Zoomed(), "too slow");
    Tap(z, 100.0f, 1000.0f, 1.0);
    Tap(z, 900.0f, 1000.0f, 1.1);
    CHECK(!z.Zoomed(), "too far apart");
    z.Touch(0, 500.0f, 1000.0f, 1, 0.0f, 0.0f, 2.0);
    z.Touch(2, 560.0f, 1000.0f, 1, 0.0f, 0.0f, 2.05);
    z.Touch(1, 560.0f, 1000.0f, 1, 0.0f, 0.0f, 2.1);
    Tap(z, 560.0f, 1000.0f, 2.2);
    CHECK(!z.Zoomed(), "a drag is not a tap");
    z.Touch(0, 500.0f, 1000.0f, 1, 0.0f, 0.0f, 3.0);
    z.Touch(1, 500.0f, 1000.0f, 1, 0.0f, 0.0f, 3.5);
    Tap(z, 500.0f, 1000.0f, 3.6);
    CHECK(!z.Zoomed(), "a long press is not a tap");
}

void TestPinch() {
    std::printf("[a pinch zooms round its midpoint, fit to 4x]\n");
    ZoomGesture z = Make();
    z.Touch(0, 400.0f, 1000.0f, 1, 0.0f, 0.0f, 0.0);
    z.Touch(5, 400.0f, 1000.0f, 2, 600.0f, 1000.0f, 0.01);
    CHECK(z.Active(), "a pinch is under way");
    z.Touch(2, 300.0f, 1000.0f, 2, 700.0f, 1000.0f, 0.1);  // twice the distance
    CHECK(Near(z.Zoom(), 2.0f), "twice the spread, twice the zoom");
    CHECK(Near(z.OffsetX(), 0.0f), "round the middle: centred");
    z.Touch(2, 0.0f, 1000.0f, 2, 1000.0f, 1000.0f, 0.2);  // 5x the distance
    CHECK(Near(z.Zoom(), ZoomGesture::kMaxZoom), "capped at 4x");
    z.Touch(2, 450.0f, 1000.0f, 2, 550.0f, 1000.0f, 0.3);
    CHECK(!z.Zoomed() && Near(z.Zoom(), 1.0f), "never below fit");
    z.Touch(6, 450.0f, 1000.0f, 1, 0.0f, 0.0f, 0.4);
    z.Touch(1, 450.0f, 1000.0f, 1, 0.0f, 0.0f, 0.5);
    CHECK(!z.Active(), "over once the fingers lift");
}

void TestPan() {
    std::printf("[one finger pans only while zoomed, within the edges]\n");
    ZoomGesture z = Make();
    z.Touch(0, 500.0f, 1000.0f, 1, 0.0f, 0.0f, 0.0);
    z.Touch(2, 300.0f, 1000.0f, 1, 0.0f, 0.0f, 0.1);
    CHECK(Near(z.OffsetX(), 0.0f) && !z.Active(), "at fit a drag is the swipe's, not a pan");
    z.Touch(1, 300.0f, 1000.0f, 1, 0.0f, 0.0f, 0.2);
    z.DoubleTap(500.0f, 1000.0f);
    z.Touch(0, 500.0f, 1000.0f, 1, 0.0f, 0.0f, 1.0);
    CHECK(z.Active(), "zoomed, a press pans");
    z.Touch(2, 400.0f, 1000.0f, 1, 0.0f, 0.0f, 1.1);
    CHECK(Near(z.OffsetX(), -100.0f), "follows the finger");
    z.Touch(2, -900.0f, 1000.0f, 1, 0.0f, 0.0f, 1.2);
    CHECK(Near(z.OffsetX(), -500.0f), "stops at the picture's edge");
    z.Touch(2, -900.0f, 600.0f, 1, 0.0f, 0.0f, 1.3);
    CHECK(Near(z.OffsetY(), 0.0f), "no room vertically: stays");
    z.Touch(1, -900.0f, 600.0f, 1, 0.0f, 0.0f, 1.4);
    z.Reset();
    CHECK(!z.Zoomed() && Near(z.OffsetX(), 0.0f), "Reset returns to fit");
}

}  // namespace

int main() {
    TestDoubleTap();
    TestDoubleTapPoint();
    TestNotDouble();
    TestPinch();
    TestPan();
    if (g_failures) {
        std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all zoom gesture checks passed\n");
    return 0;
}
