// SPDX-License-Identifier: Apache-2.0
// Target-screen depth model (#135), against hand calculations.
#include "pro/TargetScreen.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                           \
        }                                                                       \
    } while (0)
static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

using namespace mp::pro::target;

int main() {
    Screen cinema, tv, mon, tab, none;
    CHECK(PresetByName("cinema", cinema) && Near(cinema.widthM, 10.0f, 1e-6f) && Near(cinema.distanceM, 12.0f, 1e-6f));
    CHECK(PresetByName("TV", tv) && tv.name == "Home TV");
    CHECK(PresetByName("home_tv", tv) && PresetByName("Home TV", tv));
    CHECK(PresetByName("monitor", mon) && PresetByName("TABLET", tab));
    CHECK(!PresetByName("imax", none) && !none.On());

    // The Leia panel here: a full-width picture 0.3442 m wide, viewer at 0.60 m.
    //   Cinema:  (10 / 12) / (0.3442 / 0.60) = 0.83333 / 0.57367 = 1.4527 (crop)
    //   Home TV: (1.44 / 2.5) / 0.57367      = 0.576 / 0.57367   = 1.0041 (about the same)
    //   Monitor: (0.60 / 0.65) / 0.57367     = 0.92308 / 0.57367 = 1.6091
    //   Tablet:  (0.25 / 0.40) / 0.57367     = 0.625 / 0.57367   = 1.0895
    CHECK(Near(PictureScale(cinema, 0.3442f, 0.60f), 1.4527f, 0.001f));
    CHECK(Near(PictureScale(tv, 0.3442f, 0.60f), 1.0041f, 0.001f));
    CHECK(Near(PictureScale(mon, 0.3442f, 0.60f), 1.6091f, 0.001f));
    CHECK(Near(PictureScale(tab, 0.3442f, 0.60f), 1.0895f, 0.001f));
    CHECK(PictureScale(none, 0.3442f, 0.60f) == 1.0f);  // off
    CHECK(PictureScale(cinema, 0.0f, 0.60f) == 1.0f);   // panel size unknown

    // Parallax on the cinema screen: +1 % of 10 m = 100 mm behind; 2 atan(0.1 / 24) =
    // 0.0083333 rad = 28.648 arcmin; no divergence (100 mm > 65 mm: it does diverge!)
    //   divergence = 2 atan((0.100 - 0.065) / 24) = 0.0029167 rad = 0.16711 deg
    Depth d = AtParallax(cinema, 1.0f);
    CHECK(Near(d.mm, 100.0f, 0.01f));
    CHECK(Near(d.arcmin, 28.648f, 0.01f));
    CHECK(Near(d.divergenceDeg, 0.16711f, 0.0005f));
    // -2 % in front: -200 mm, -57.29 arcmin, never divergent.
    d = AtParallax(cinema, -2.0f);
    CHECK(Near(d.mm, -200.0f, 0.01f) && Near(d.arcmin, -57.293f, 0.01f) && d.divergenceDeg == 0.0f);
    // +1 % on the monitor: 6 mm, 2 atan(0.006 / 1.3) = 31.73 arcmin, nowhere near diverging.
    d = AtParallax(mon, 1.0f);
    CHECK(Near(d.mm, 6.0f, 0.001f) && Near(d.arcmin, 31.733f, 0.01f) && d.divergenceDeg == 0.0f);

    // Divergence cap 0.5 deg on the cinema: P = 0.065 + 2 * 12 * tan(0.25 deg) = 0.16972 m
    // = 1.6972 % of 10 m. On the TV: 0.065 + 5 tan(0.25 deg) = 0.086817 m = 6.029 % of 1.44 m.
    CHECK(Near(DivergenceLimitPct(cinema), 1.6972f, 0.001f));
    CHECK(Near(DivergenceLimitPct(tv), 6.029f, 0.002f));
    CHECK(Near(AtParallax(cinema, DivergenceLimitPct(cinema)).divergenceDeg, 0.5f, 1e-4f));
    CHECK(DivergenceLimitPct(none) == 0.0f);

    if (g_fail) std::fprintf(stderr, "target_screen_test: %d failure(s)\n", g_fail);
    else std::printf("target_screen_test: OK\n");
    return g_fail ? 1 : 0;
}
