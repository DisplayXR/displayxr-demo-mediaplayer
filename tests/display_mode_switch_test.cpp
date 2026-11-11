// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayModeSwitch: it flips between the flat mode and the 3D mode, goes
// back to the 3D mode last in force, prefers a mode that turns the lens on, and is not
// available without both a 2D and a 3D mode.

#include "xr/DisplayModeSwitch.h"

#include <cstdio>

using mp::DisplayModeSwitch;
using Mode = DisplayModeSwitch::Mode;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// The NP02J: [0] 2D, [1] LeiaSR.
void TabletFlips() {
    DisplayModeSwitch sw;
    sw.SetModes({{0, 1, false}, {1, 2, true}});
    sw.SetActive(1);
    CHECK(sw.Available(), "2D + 3D: available");
    CHECK(sw.Is3D(), "LeiaSR is 3D");
    CHECK(sw.Flipped() == 0, "3D flips to 2D");
    sw.SetActive(0);
    CHECK(!sw.Is3D(), "2D is not 3D");
    CHECK(sw.Flipped() == 1, "2D flips back to 3D");
}

// sim_display: 2D, Anaglyph, Cropped SBS, Squeezed SBS, Quad, Interlaced.
void BackToLast3D() {
    DisplayModeSwitch sw;
    sw.SetModes({{0, 1, false}, {1, 2, false}, {2, 2, false}, {3, 2, false}, {4, 4, false},
                 {5, 2, true}});
    sw.SetActive(3);
    sw.SetActive(0);
    CHECK(sw.Flipped() == 3, "2D goes back to the 3D mode last in force");
}

void StartsIn2D() {
    DisplayModeSwitch sw;
    sw.SetModes({{0, 1, false}, {1, 2, false}, {2, 2, true}});
    sw.SetActive(0);
    CHECK(sw.Flipped() == 2, "no 3D mode seen yet: the one that turns the lens on");
    sw.SetModes({{0, 1, false}, {1, 2, false}});
    sw.SetActive(0);
    CHECK(sw.Flipped() == 1, "no lens mode: any stereo mode");
}

void FlatRule() {
    DisplayModeSwitch sw;
    sw.SetModes({{0, 2, true}, {1, 1, true}, {2, 1, false}});
    CHECK(sw.Flat() == 2, "mono with the lens off wins");
    sw.SetModes({{0, 2, true}, {1, 1, true}});
    CHECK(sw.Flat() == 1, "mono with the lens on beats stereo");
}

void NotAvailable() {
    DisplayModeSwitch sw;
    CHECK(!sw.Available(), "no modes");
    sw.SetModes({{0, 2, true}});
    sw.SetActive(0);
    CHECK(!sw.Available(), "3D only");
    sw.SetModes({{0, 1, false}});
    sw.SetActive(0);
    CHECK(!sw.Available(), "2D only");
    CHECK(sw.Flipped() == -1, "2D only: nothing to flip to");
}

}  // namespace

int main() {
    TabletFlips();
    BackToLast3D();
    StartsIn2D();
    FlatRule();
    NotAvailable();
    if (g_failures) {
        std::fprintf(stderr, "display_mode_switch_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("display_mode_switch_test: OK\n");
    return 0;
}
