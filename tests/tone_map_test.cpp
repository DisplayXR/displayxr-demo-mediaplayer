// SPDX-License-Identifier: Apache-2.0
// Unit tests for HDR on the SDR panel (#134): color/ToneMap.h (the shader's HdrToDisplay twin),
// the EXR PQ encode, and the push-word packing. Reference values from SMPTE ST 2084, ITU-R
// BT.2100 and BT.2408.
#include "color/ToneMap.h"
#include "rhi/PushPack.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace mp::color;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// ST 2084 code values (BT.2408 Table 1 / the PQ spec): 100 nits = 0.508, 203 = 0.581,
// 1000 = 0.752, 10000 = 1.
static void TestPq() {
    CHECK(Near(NitsToPq(0.0f), 0.0f, 1e-4f));
    CHECK(Near(NitsToPq(100.0f), 0.5081f, 5e-4f));
    CHECK(Near(NitsToPq(203.0f), 0.5806f, 5e-4f));
    CHECK(Near(NitsToPq(1000.0f), 0.7518f, 5e-4f));
    CHECK(Near(NitsToPq(10000.0f), 1.0f, 1e-5f));
    for (float n : {0.01f, 1.0f, 48.0f, 203.0f, 1000.0f, 4000.0f, 10000.0f})
        CHECK(Near(PqToNits(NitsToPq(n)), n, n * 1e-3f + 1e-4f));
}

// BT.2100 HLG: signal 0.5 is scene 1/12, the curve is continuous there, signal 1 is scene 1.
static void TestHlg() {
    CHECK(Near(HlgToScene(0.0f), 0.0f, 1e-7f));
    CHECK(Near(HlgToScene(0.5f), 1.0f / 12.0f, 1e-6f));
    CHECK(Near(HlgToScene(0.5001f), HlgToScene(0.5f), 1e-4f));
    CHECK(Near(HlgToScene(1.0f), 1.0f, 1e-3f));
    CHECK(Near(HlgSystemGamma(1000.0f), 1.2f, 1e-6f));
    // HLG 75 % is BT.2408's reference white, ~203 nits on a 1000-nit display (grey: Ys = E).
    HdrParams p;
    p.transfer = Transfer::HLG;
    p.rollOff = false;
    float c[3] = {0.75f, 0.75f, 0.75f};
    HdrToDisplay(p, c);
    CHECK(Near(SrgbDecode(c[0]), 1.0f, 0.03f));  // 203 nits -> SDR white, within 3 %
}

static void TestBt2390() {
    // Below the knee (~80 nits for 1000 -> 203) it is the identity.
    CHECK(Near(Bt2390Nits(50.0f, 1000.0f, 203.0f), 50.0f, 0.05f));
    CHECK(Near(Bt2390Nits(10.0f, 1000.0f, 203.0f), 10.0f, 0.01f));
    // The source peak lands on the target peak; nothing above it.
    CHECK(Near(Bt2390Nits(1000.0f, 1000.0f, 203.0f), 203.0f, 0.2f));
    CHECK(Near(Bt2390Nits(4000.0f, 1000.0f, 203.0f), 203.0f, 0.2f));
    // Monotonic, never above the target.
    float prev = 0.0f;
    bool mono = true, capped = true;
    for (int i = 1; i <= 2000; ++i) {
        const float o = Bt2390Nits((float)i, 1000.0f, 203.0f);
        mono = mono && o >= prev - 1e-3f;
        capped = capped && o <= 203.0f + 0.2f;
        prev = o;
    }
    CHECK(mono);
    CHECK(capped);
    // A source no brighter than the target is only clipped.
    CHECK(Near(Bt2390Nits(150.0f, 200.0f, 203.0f), 150.0f, 1e-3f));
}

static void TestHdrToDisplay() {
    HdrParams p;
    p.transfer = Transfer::PQ;
    p.peakNits = 1000.0f;
    p.rollOff = true;
    // The source peak (1000-nit white) is SDR white, not clipped earlier...
    float w[3] = {NitsToPq(1000.0f), NitsToPq(1000.0f), NitsToPq(1000.0f)};
    HdrToDisplay(p, w);
    CHECK(Near(w[0], 1.0f, 2e-3f) && Near(w[1], 1.0f, 2e-3f));
    // ... while 500 nits is still below it: highlight detail survives the roll-off.
    float h[3] = {NitsToPq(500.0f), NitsToPq(500.0f), NitsToPq(500.0f)};
    HdrToDisplay(p, h);
    CHECK(h[0] < 0.995f && h[0] > 0.9f);
    // Shadows and mid-tones are untouched: 20 nits = 20/203 of SDR white.
    float s[3] = {NitsToPq(20.0f), NitsToPq(20.0f), NitsToPq(20.0f)};
    HdrToDisplay(p, s);
    CHECK(Near(s[0], SrgbEncode(20.0f / 203.0f), 2e-3f));
    // Without the roll-off, everything from reference white up clips.
    p.rollOff = false;
    float c[3] = {NitsToPq(500.0f), NitsToPq(203.0f), NitsToPq(100.0f)};
    HdrToDisplay(p, c);
    CHECK(Near(c[0], 1.0f, 1e-4f) && Near(c[1], 1.0f, 2e-3f) && c[2] < 0.9f);
    // Hue: the roll-off scales R, G, B together (max(R,G,B)), so a bright saturated colour
    // keeps its linear ratios.
    p.rollOff = true;
    float col[3] = {NitsToPq(800.0f), NitsToPq(400.0f), NitsToPq(100.0f)};
    HdrToDisplay(p, col);
    const float r = SrgbDecode(col[0]), g = SrgbDecode(col[1]), b = SrgbDecode(col[2]);
    CHECK(Near(g / r, 0.5f, 0.01f) && Near(b / r, 0.125f, 0.01f));
}

// Scene-linear EXR: 1.0 is reference white; 4.0 (two stops over) clips without the roll-off,
// and exposure acts before the clip, so -2 stops brings it back to white and 2.0 to 0.5.
static void TestLinearExposure() {
    std::vector<float> lut;
    BuildLinearPqTable(lut);
    HdrParams p;
    p.transfer = Transfer::LinearPq;
    p.rollOff = false;
    auto show = [&](float v, float ev) {
        float c[3] = {EncodeLinearPq(lut.data(), v), EncodeLinearPq(lut.data(), v), EncodeLinearPq(lut.data(), v)};
        p.exposure = ev;
        HdrToDisplay(p, c);
        return c[0];
    };
    CHECK(Near(show(4.0f, 0.0f), 1.0f, 1e-4f));
    CHECK(Near(show(2.0f, 0.0f), 1.0f, 1e-4f));
    CHECK(Near(show(4.0f, -2.0f), 1.0f, 2e-3f));
    CHECK(Near(show(2.0f, -2.0f), SrgbEncode(0.5f), 2e-3f));
    CHECK(Near(show(0.18f, 0.0f), SrgbEncode(0.18f), 2e-3f));  // mid grey as before
    // In range, the PQ store shows a value as the old sRGB store did: within half an 8-bit code.
    float worst = 0.0f;
    for (int i = 0; i <= 1000; ++i) {
        const float v = i / 1000.0f;
        worst = std::fmax(worst, std::fabs(show(v, 0.0f) - SrgbEncode(v)) * 255.0f);
    }
    std::printf("linear->PQ->display vs sRGB: worst %.3f 8-bit codes\n", worst);
    CHECK(worst < 0.5f);
    // NaN and negatives store as black (PQ's own zero, c1^m2 ~ 7e-7), above ~49x clips at 1.
    CHECK(Near(EncodeLinearPq(lut.data(), std::nanf("")), NitsToPq(0.0f), 1e-7f));
    CHECK(Near(EncodeLinearPq(lut.data(), -3.0f), NitsToPq(0.0f), 1e-7f));
    CHECK(Near(EncodeLinearPq(lut.data(), 1e6f), 1.0f, 1e-5f));
}

// The table encode against the exact ST 2084 maths, in 16-bit codes (the P016 store).
static void TestLinearPqTable() {
    std::vector<float> lut;
    BuildLinearPqTable(lut);
    float worst = 0.0f;
    for (int i = 0; i <= 4000; ++i) {
        const float v = kLinearPqMax * std::pow(i / 4000.0f, 4.0f) + 1e-6f * i;
        const float exact = (float)NitsToPqD((double)v * kRefWhiteNits);
        worst = std::fmax(worst, std::fabs(EncodeLinearPq(lut.data(), v) - exact) * 65535.0f);
    }
    std::printf("PQ table vs exact: worst %.3f 16-bit codes\n", worst);
    CHECK(worst < 1.0f);
}

// An OCIO look on scene-linear EXR: the LUT gets PQ code of the linear light, after the
// exposure and unclipped (no roll-off, no sRGB encode), so 4.0 survives as 4.0.
static void TestPqShaper() {
    HdrParams p;
    p.transfer = Transfer::LinearPq;
    p.pqShaper = true;
    p.rollOff = true;  // ignored with the shaper
    const float code = NitsToPq(4.0f * kRefWhiteNits);
    float c[3] = {code, code, code};
    HdrToDisplay(p, c);
    CHECK(Near(c[0], code, 1e-4f));
    p.exposure = -1.0f;
    float e[3] = {code, code, code};
    HdrToDisplay(p, e);
    CHECK(Near(PqToNits(e[0]) / kRefWhiteNits, 2.0f, 2e-3f));
}

static void TestGamut() {
    float w[3] = {1.0f, 1.0f, 1.0f};
    Bt2020To709(w);  // D65 white stays white
    CHECK(Near(w[0], 1.0f, 1e-4f) && Near(w[1], 1.0f, 1e-4f) && Near(w[2], 1.0f, 1e-4f));
    float g[3] = {0.0f, 1.0f, 0.0f};
    Bt2020To709(g);  // BT.2020 green is outside BT.709: red and blue go negative
    CHECK(g[0] < 0.0f && g[2] < 0.0f && g[1] > 1.0f);
}

static void TestPack() {
    // SDR packs to the matrix alone: an SDR frame's push word is unchanged.
    for (int m = 0; m < 3; ++m) CHECK(mp::PackColourWord(m, 0, true, true, 4000.0f) == m);
    const int32_t w = mp::PackColourWord(2, 2, true, true, 1000.0f);
    CHECK((w & 3) == 2);
    CHECK(((w >> 4) & 3) == 2);
    CHECK(((w >> 6) & 1) == 1);
    CHECK(((w >> 7) & 1) == 1);
    CHECK(((uint32_t)w >> 16) == 1000u);
    CHECK(((uint32_t)mp::PackColourWord(0, 3, false, false, 50000.0f) >> 16) == 10000u);
    CHECK(((mp::PackColourWord(0, 1, false, false, 0.0f) >> 6) & 3) == 0);
    CHECK(((mp::PackColourWord(0, 3, false, false, 100.0f, true) >> 8) & 1) == 1);
    CHECK(((mp::PackColourWord(0, 3, false, false, 100.0f) >> 8) & 1) == 0);
}

int main() {
    TestPq();
    TestHlg();
    TestBt2390();
    TestHdrToDisplay();
    TestLinearExposure();
    TestLinearPqTable();
    TestPqShaper();
    TestGamut();
    TestPack();
    if (g_fail) {
        std::fprintf(stderr, "tone_map_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("tone_map_test: all passed\n");
    return 0;
}
