// SPDX-License-Identifier: Apache-2.0
//
// Rig checks (Media Player Pro, phase 2) against the synthetic rig, whose answers are
// known: its discs sit at fixed parallaxes (TestSignalSource::kDisparity), and the
// faults are dialled in. A clean rig must read as clean; each fault must be found.

#include "pro/LivePixels.h"
#include "pro/RigCheck.h"
#include "pro/TestSignalSource.h"

#include <cmath>
#include <cstdio>

using namespace mp;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

rig::Result MeasureTest(const TestSignalConfig& cfg, int64_t frame = 30) {
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, frame, px);
    FrameRing::Frame f;
    f.plane[0] = px.y;
    f.plane[1] = px.uv;
    f.width = px.width;
    f.height = px.height;
    f.format = PixFormat::NV12;
    f.fullRange = false;
    rig::EyePair e;
    if (!rig::Downsample(f, StereoLayout::SbsFull, 640, e)) return {};
    return rig::Measure(e);
}

void CleanRig() {
    TestSignalConfig cfg;
    const rig::Result r = MeasureTest(cfg);
    std::printf("  clean: ok=%d matches=%d vertical=%.2f px roll=%.2f near=%.2f%% far=%.2f%% dE=%.2f\n", r.ok,
                r.matches, r.verticalPx, r.rollDeg, r.nearPct, r.farPct, r.deltaE);
    CHECK(r.ok && r.matches >= 10, "enough matches on the (mostly flat) test scene");
    CHECK(std::fabs(r.verticalPx) < 0.5f, "clean rig: no vertical error");
    CHECK(std::fabs(r.rollDeg) < 0.2f, "clean rig: no roll");
    CHECK(std::fabs(r.nearPct - TestSignalSource::kDisparity[0] * 100.f) < 0.35f, "near = the front disc (-2%)");
    CHECK(std::fabs(r.farPct - TestSignalSource::kDisparity[3] * 100.f) < 0.35f, "far = the back disc (+1.2%)");
    CHECK(r.deltaE < 1.0f, "clean rig: eyes match in colour");
}

void VerticalFault() {
    TestSignalConfig cfg;
    cfg.vShiftPx = 6.0f;
    const rig::Result r = MeasureTest(cfg);
    std::printf("  vshift 6: vertical=%.2f px\n", r.verticalPx);
    CHECK(r.ok && std::fabs(r.verticalPx - 6.0f) < 0.75f, "6 px vertical shift measured");
    cfg.vShiftPx = -2.5f;
    const rig::Result r2 = MeasureTest(cfg);
    std::printf("  vshift -2.5: vertical=%.2f px\n", r2.verticalPx);
    CHECK(r2.ok && std::fabs(r2.verticalPx + 2.5f) < 0.75f, "-2.5 px shift measured, sign kept");
}

void ColourFault() {
    TestSignalConfig cfg;
    cfg.gainR = 1.15f;
    const rig::Result r = MeasureTest(cfg);
    std::printf("  right gain 1.15: dE=%.2f\n", r.deltaE);
    CHECK(r.deltaE > 2.0f, "a 15% brighter right eye is a visible colour mismatch");
}

void DepthScale() {
    TestSignalConfig cfg;
    cfg.depthScale = 1.5f;
    const rig::Result r = MeasureTest(cfg);
    std::printf("  depth x1.5: near=%.2f%% far=%.2f%%\n", r.nearPct, r.farPct);
    CHECK(std::fabs(r.nearPct + 3.0f) < 0.45f, "near scales to -3%");
}

void MonoRefused() {
    FrameRing::Frame f;
    f.width = 640;
    f.height = 360;
    f.plane[0].assign(640 * 360, 100);
    f.plane[1].assign(640 * 180, 128);
    f.format = PixFormat::NV12;
    rig::EyePair e;
    CHECK(!rig::Downsample(f, StereoLayout::Mono, 640, e), "mono is not measured");
}

void DeltaE() {
    const float a[3] = {0.5f, 0.5f, 0.5f};
    CHECK(rig::DeltaE76(a, a) < 1e-4f, "same colour = 0");
    const float b[3] = {0.52f, 0.5f, 0.5f};
    const float d = rig::DeltaE76(a, b);
    CHECK(d > 0.5f && d < 3.0f, "a small red shift is a small delta-E");
}

}  // namespace

int main() {
    CleanRig();
    VerticalFault();
    ColourFault();
    DepthScale();
    MonoRefused();
    DeltaE();
    if (g_failures) {
        std::fprintf(stderr, "rig_check_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rig_check_test: all passed\n");
    return 0;
}
