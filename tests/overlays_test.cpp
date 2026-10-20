// SPDX-License-Identifier: Apache-2.0
//
// pro::FrameLineRect: frame lines letterbox a wider aspect, pillarbox a narrower one, stay
// centred, and leave the picture whole at its own aspect. mp::PackWipeGamma: the wipe
// position and display gamma survive the shared push-block word.

#include "pro/Overlays.h"
#include "rhi/PushPack.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int Find(const char* name) {
    for (int i = 0; i < FrameLineCount(); ++i)
        if (std::strcmp(FrameLineName(i), name) == 0) return i;
    return -1;
}

}  // namespace

int main() {
    const float hd = 16.0f / 9.0f;
    float x, y, w, h;
    FrameLineRect(hd, 2.39f, x, y, w, h);
    CHECK(Near(x, 0) && Near(w, 1) && Near(h, hd / 2.39f) && Near(y, (1 - hd / 2.39f) / 2), "2.39 letterboxes 16:9");
    FrameLineRect(hd, 4.0f / 3.0f, x, y, w, h);
    CHECK(Near(y, 0) && Near(h, 1) && Near(w, 0.75f) && Near(x, 0.125f), "4:3 pillarboxes 16:9");
    FrameLineRect(hd, hd, x, y, w, h);
    CHECK(Near(x, 0) && Near(y, 0) && Near(w, 1) && Near(h, 1), "the picture's own aspect: the whole picture");
    FrameLineRect(hd, 9.0f / 16.0f, x, y, w, h);
    CHECK(Near(w, (9.0f / 16.0f) / hd) && Near(x + w / 2, 0.5f), "9:16 vertical, centred");
    FrameLineRect(hd, 0.0f, x, y, w, h);
    CHECK(Near(w, 1) && Near(h, 1), "off: no line");

    CHECK(Find("Off") == 0 && FrameLineAspect(0) == 0.0f, "index 0 is off");
    CHECK(Find("2.39") > 0 && Find("1.85") > 0 && Find("16:9") > 0 && Find("9:16") > 0 && Find("4:3") > 0,
          "the plan's frame lines are all offered");
    CHECK(Near(DesqueezeFactor(0), 1.0f) && DesqueezeCount() == 5 && Near(DesqueezeFactor(DesqueezeCount() - 1), 2.0f),
          "desqueeze 1.0 .. 2.0");
    // Wipe position + display gamma share one push-block word (the block is at 128 bytes).
    for (float pos : {0.0f, 0.25f, 0.5f, 0.731f, 1.0f})
        for (float gam : {0.25f, 0.8f, 1.0f, 2.2f, 4.0f}) {
            float p2 = -1, g2 = -1;
            mp::UnpackWipeGamma(mp::PackWipeGamma(pos, gam), p2, g2);
            CHECK(std::fabs(p2 - pos) < 2e-5f && std::fabs(g2 - gam) < 2e-4f, "wipe / gamma round trip");
        }
    float p3, g3;
    mp::UnpackWipeGamma(mp::PackWipeGamma(-1.0f, 9.0f), p3, g3);
    CHECK(p3 == 0.0f && Near(g3, 4.0f), "out-of-range wipe / gamma clamp");
    mp::UnpackWipeGamma(mp::PackWipeGamma(0.5f, 1.0f), p3, g3);
    CHECK(Near(g3, 1.0f), "gamma 1 survives exactly (the shader skips the pow)");
    if (g_failures) {
        std::fprintf(stderr, "overlays_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("overlays_test: all passed\n");
    return 0;
}
