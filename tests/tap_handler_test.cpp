// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for TapHandler: the chrome shows for a while after a touch and while held, a
// tap on the picture hides a shown chrome and shows a hidden one, a tap on the chrome or a
// drag changes nothing, a hidden chrome stays hidden through touches and wakes, and Show
// brings it back.

#include "ui/TapHandler.h"

#include <cstdio>

using mp::ui::TapHandler;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

TapHandler Make() {
    TapHandler h;
    h.SetSlop(10.0f);
    return h;
}

// A tap at (500, 500) from t to t + 0.1.
void Tap(TapHandler& h, double t, bool onChrome = false) {
    h.Press(500.0f, 500.0f, t, onChrome);
    h.Release(502.0f, 501.0f, t + 0.1);
}

void TestTimeout() {
    std::printf("[shows for a while after a touch]\n");
    TapHandler h = Make();
    CHECK(!h.Shown(0.0), "hidden at start");
    h.Wake(1.0);
    CHECK(h.Shown(1.0) && h.Shown(3.9), "shown after a wake");
    CHECK(!h.Shown(4.1), "hidden once the time is up");
    h.SetHeld(true);
    CHECK(h.Shown(100.0), "shown while held");
}

void TestTapToggles() {
    std::printf("[a tap on the picture toggles]\n");
    TapHandler h = Make();
    h.Wake(0.0);
    CHECK(h.Press(500.0f, 500.0f, 1.0, false), "a press while shown is the chrome's");
    h.Release(500.0f, 500.0f, 1.1);
    CHECK(!h.Shown(1.2), "a tap while shown hides it");
    CHECK(!h.Press(500.0f, 500.0f, 2.0, false), "a press while hidden is not the chrome's");
    h.Release(500.0f, 500.0f, 2.1);
    CHECK(h.Shown(2.2), "a tap while hidden shows it");
    CHECK(h.Shown(5.0) && !h.Shown(5.2), "and it times out from the tap");
    Tap(h, 6.0);
    CHECK(h.Shown(6.2), "a tap after the time out shows it");
}

void TestTapOnChrome() {
    std::printf("[a tap on the chrome is the chrome's]\n");
    TapHandler h = Make();
    h.Wake(0.0);
    Tap(h, 1.0, true);
    CHECK(h.Shown(1.2), "a tap on a control keeps it shown");
    h.Wake(10.0);
    h.Release(500.0f, 500.0f, 10.0);  // no press: nothing to toggle
    CHECK(h.Shown(10.1), "a release with no press changes nothing");
}

void TestDragAndCancel() {
    std::printf("[a drag or a cancel is no tap]\n");
    TapHandler h = Make();
    h.Wake(0.0);
    h.Press(500.0f, 500.0f, 1.0, false);
    h.Move(530.0f, 500.0f, 1.05);
    h.Release(500.0f, 500.0f, 1.1);
    CHECK(h.Shown(1.2), "a drag back to the start is still a drag");
    h.Press(500.0f, 500.0f, 2.0, false);
    h.Cancel(2.1);
    h.Release(500.0f, 500.0f, 2.2);
    CHECK(h.Shown(2.3), "a cancelled press is no tap");
}

void TestHidden() {
    std::printf("[a hidden chrome stays hidden until a tap or Show]\n");
    TapHandler h = Make();
    h.Wake(0.0);
    Tap(h, 1.0);
    CHECK(!h.Shown(1.2), "hidden by the tap");
    h.Wake(2.0);
    CHECK(!h.Shown(2.1), "a wake leaves it hidden");
    h.Press(500.0f, 500.0f, 3.0, false);
    h.Move(800.0f, 500.0f, 3.1);
    h.Release(800.0f, 500.0f, 3.2);
    CHECK(!h.Shown(3.3), "a swipe leaves it hidden");
    h.SetHeld(true);
    CHECK(!h.Shown(3.4), "held leaves it hidden");
    Tap(h, 4.0);
    CHECK(h.Shown(4.2), "a tap brings it back");
    Tap(h, 5.0);
    CHECK(!h.Shown(5.2), "a tap hides it while held");
    h.Show(6.0);
    CHECK(h.Shown(6.1), "Show brings it back");
}

} // namespace

int main() {
    TestTimeout();
    TestTapToggles();
    TestTapOnChrome();
    TestDragAndCancel();
    TestHidden();
    if (g_failures) {
        std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all tap handler checks passed\n");
    return 0;
}
