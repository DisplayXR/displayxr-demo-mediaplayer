// SPDX-License-Identifier: Apache-2.0
//
// Rig checks (Media Player Pro, phase 2) against the synthetic rig, whose answers are
// known: its discs sit at fixed parallaxes (TestSignalSource::kDisparity), and the
// faults are dialled in. A clean rig must read as clean; each fault must be found.

#include "color/Cdl.h"
#include "pro/LivePixels.h"
#include "pro/RigCheck.h"
#include "pro/TestSignalSource.h"

#include "media/LiveProducer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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

FrameRing::Frame ToFrame(const livepix::Nv12& px) {
    FrameRing::Frame f;
    f.plane[0] = px.y;
    f.plane[1] = px.uv;
    f.width = px.width;
    f.height = px.height;
    f.format = PixFormat::NV12;
    f.fullRange = false;
    return f;
}

// Swap the two halves of an SBS frame (an R|L master).
livepix::Nv12 SwapHalves(const livepix::Nv12& px) {
    livepix::Nv12 o = px;
    const int h = px.width / 2;
    for (int y = 0; y < px.height; ++y)
        for (int x = 0; x < h; ++x) std::swap(o.y[(size_t)y * px.width + x], o.y[(size_t)y * px.width + x + h]);
    for (int y = 0; y < px.height / 2; ++y)
        for (int x = 0; x < h; ++x) std::swap(o.uv[(size_t)y * px.width + x], o.uv[(size_t)y * px.width + x + h]);
    return o;
}

// Squeeze an SBS-full frame into half-SBS (every eye at half its width).
livepix::Nv12 Squeeze(const livepix::Nv12& px) {
    livepix::Nv12 o;
    o.Resize(px.width / 2, px.height);
    for (int y = 0; y < px.height; ++y)
        for (int x = 0; x < o.width; ++x) o.y[(size_t)y * o.width + x] = px.y[(size_t)y * px.width + 2 * x];
    for (int y = 0; y < px.height / 2; ++y)
        for (int x = 0; x < o.width / 2; ++x) {
            o.uv[(size_t)y * o.width + 2 * x] = px.uv[(size_t)y * px.width + 4 * x];
            o.uv[(size_t)y * o.width + 2 * x + 1] = px.uv[(size_t)y * px.width + 4 * x + 1];
        }
    return o;
}

rig::Result MeasureTest(const TestSignalConfig& cfg, int64_t frame = 30) {
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, frame, px);
    rig::EyePair e;
    if (!rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e)) return {};
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

void EyeSwap() {
    TestSignalConfig cfg;
    cfg.vShiftPx = 4.0f;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    const livepix::Nv12 sw = SwapHalves(px);  // an R|L master of the same rig
    rig::EyePair e;
    rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e);
    const rig::Result plain = rig::Measure(e);
    rig::Downsample(ToFrame(sw), StereoLayout::SbsFull, 640, e, /*swapEyes=*/true);
    const rig::Result fixed = rig::Measure(e);
    rig::Downsample(ToFrame(sw), StereoLayout::SbsFull, 640, e, /*swapEyes=*/false);
    const rig::Result wrong = rig::Measure(e);
    std::printf("  eye swap: plain near=%.2f far=%.2f v=%.2f | swapped+flag near=%.2f far=%.2f v=%.2f | no flag near=%.2f far=%.2f v=%.2f\n",
                plain.nearPct, plain.farPct, plain.verticalPx, fixed.nearPct, fixed.farPct, fixed.verticalPx,
                wrong.nearPct, wrong.farPct, wrong.verticalPx);
    CHECK(plain.ok && fixed.ok && wrong.ok, "all three measure");
    CHECK(std::fabs(fixed.nearPct - plain.nearPct) < 0.1f && std::fabs(fixed.farPct - plain.farPct) < 0.1f,
          "swapped input + swap flag = same depth range");
    CHECK(std::fabs(fixed.verticalPx - plain.verticalPx) < 0.3f, "swapped input + swap flag = same vertical sign");
    CHECK(std::fabs(wrong.nearPct + plain.farPct) < 0.35f && std::fabs(wrong.farPct + plain.nearPct) < 0.35f,
          "swapped input without the flag = depth inverted");
    CHECK(wrong.verticalPx < -3.0f, "swapped input without the flag = vertical sign flipped");
}

void RollFault() {
    TestSignalConfig cfg;
    cfg.rollDeg = 0.6f;
    const rig::Result r = MeasureTest(cfg);
    std::printf("  roll 0.6 deg: roll=%.2f deg vertical=%.2f px matches=%d\n", r.rollDeg, r.verticalPx, r.matches);
    CHECK(r.ok && std::fabs(r.rollDeg - 0.6f) < 0.15f, "0.6 deg right-eye roll measured");
    // the same rig delivered half-SBS: the eyes are squeezed, the roll on screen is the same
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    rig::EyePair e;
    CHECK(rig::Downsample(ToFrame(Squeeze(px)), StereoLayout::SbsHalf, 640, e), "half-SBS downsamples");
    const rig::Result h = rig::Measure(e);
    std::printf("  roll 0.6 deg half-SBS: roll=%.2f deg (stretch %.0f)\n", h.rollDeg, e.xStretch);
    CHECK(h.ok && std::fabs(h.rollDeg - 0.6f) < 0.15f, "half-SBS roll is the displayed roll, not 2x");
}

// --- Scale (#269) ---------------------------------------------------------------------

void ScaleFault() {
    // a zoomed right eye: the zoom is the vertical offsets' slope over y, unbiased by depth
    for (float s : {0.99f, 1.00f, 1.01f, 1.02f}) {
        TestSignalConfig cfg;
        cfg.scale = s;
        const rig::Result r = MeasureTest(cfg);
        std::printf("  scale %.2f: scale=%+.3f%% (measured %d) roll=%.3f vertical=%.2f px keystone=%.2f px matches=%d\n",
                    s, r.scalePct, r.scaleMeasured, r.rollDeg, r.verticalPx, r.keystonePx, r.matches);
        char m[96];
        std::snprintf(m, sizeof(m), "scale %.2f recovered within +-0.1 %%", s);
        CHECK(r.ok && r.scaleMeasured && std::fabs(r.scalePct - (s - 1.f) * 100.f) < 0.1f, m);
        std::snprintf(m, sizeof(m), "scale %.2f: no roll or vertical error leaks out of it", s);
        CHECK(std::fabs(r.rollDeg) < 0.05f && std::fabs(r.verticalPx) < 0.5f, m);
    }
    // roll and zoom together: each is its own term
    TestSignalConfig both;
    both.rollDeg = 0.5f;
    both.scale = 1.01f;
    const rig::Result rb = MeasureTest(both);
    std::printf("  roll 0.5 + scale 1.01: roll=%.3f deg scale=%+.3f%% vertical=%.2f px\n", rb.rollDeg, rb.scalePct,
                rb.verticalPx);
    CHECK(rb.ok && std::fabs(rb.rollDeg - 0.5f) < 0.05f, "roll 0.5 deg with a zoom: roll within +-0.05 deg");
    CHECK(rb.scaleMeasured && std::fabs(rb.scalePct - 1.0f) < 0.1f, "roll 0.5 deg with a zoom: scale within +-0.1 %");
    // depth only (the confound): disparities x0.5 .. x2 move dx, never the zoom
    for (float d : {0.5f, 1.0f, 2.0f}) {
        TestSignalConfig cfg;
        cfg.depthScale = d;
        const rig::Result r = MeasureTest(cfg);
        std::printf("  depth x%.1f: scale=%+.3f%% (measured %d) near=%.2f%%\n", d, r.scalePct, r.scaleMeasured, r.nearPct);
        char m[96];
        std::snprintf(m, sizeof(m), "depth x%.1f leaves the scale at 0 (+-0.05 %%)", d);
        CHECK(r.ok && r.scaleMeasured && std::fabs(r.scalePct) < 0.05f, m);
    }
    // the same zoom delivered half-SBS: the reading is unitless, so it is the same
    TestSignalConfig cfg;
    cfg.scale = 1.01f;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    rig::EyePair e;
    rig::Downsample(ToFrame(Squeeze(px)), StereoLayout::SbsHalf, 640, e);
    const rig::Result h = rig::Measure(e);
    std::printf("  scale 1.01 half-SBS: scale=%+.3f%% roll=%.3f\n", h.scalePct, h.rollDeg);
    CHECK(h.ok && h.scaleMeasured && std::fabs(h.scalePct - 1.0f) < 0.15f, "half-SBS: the same scale");
}

// Matches only in a band across the middle cannot tell a zoom: not measured, and the rest
// of the fit (offset + roll) still is.
void ScaleNeedsCoverage() {
    TestSignalConfig cfg;
    cfg.scale = 1.01f;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    rig::EyePair e;
    rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e);
    // flatten everything outside rows 40 % .. 60 % (both eyes)
    for (std::vector<float>* eye : {&e.left, &e.right})
        for (int y = 0; y < e.h; ++y)
            if (y < e.h * 2 / 5 || y > e.h * 3 / 5)
                std::fill(eye->begin() + (size_t)y * e.eyeW, eye->begin() + (size_t)(y + 1) * e.eyeW, 80.f);
    rig::Options o;
    o.minMatches = 4;
    const rig::Result r = rig::Measure(e, o);
    std::printf("  scale, middle band only: ok=%d matches=%d scale measured=%d\n", r.ok, r.matches, r.scaleMeasured);
    CHECK(r.ok && !r.scaleMeasured && r.scalePct == 0.f, "a middle band of matches: scale not measured");
}

// The vertical search follows the last fit: an offset past +-maxDy (here 30 px = 10 analysis
// rows at 1080p) is lost plain, and found when the search is seeded near it.
void SeededSearch() {
    TestSignalConfig cfg;
    cfg.vShiftPx = 30.f;
    cfg.scale = 1.02f;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    rig::EyePair e;
    rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e);
    const rig::Result plain = rig::Measure(e);
    rig::Result prev;  // what the last frame measured: about right, not exact
    prev.ok = true;
    prev.verticalPx = 27.f;
    prev.scaleMeasured = true;
    prev.scalePctRaw = 1.5f;
    rig::Options o;
    o.SeedFrom(prev, e);
    const rig::Result seeded = rig::Measure(e, o);
    std::printf("  vshift 30 + scale 1.02: plain ok=%d v=%.2f | seeded ok=%d v=%.2f scale=%+.3f%%\n", plain.ok,
                plain.verticalPx, seeded.ok, seeded.verticalPx, seeded.scalePct);
    CHECK(!plain.ok || std::fabs(plain.verticalPx - 30.f) > 1.f, "past the search range, unseeded: not found");
    CHECK(seeded.ok && std::fabs(seeded.verticalPx - 30.f) < 0.75f, "seeded: the 30 px offset is found");
    CHECK(seeded.scaleMeasured && std::fabs(seeded.scalePct - 2.0f) < 0.1f, "seeded: the 2 % zoom is found");
}

void ScaleAlert() {
    for (bool on : {true, false}) {
        rig::RigMonitor m;
        rig::RigMonitor::Checks c;
        c.scale = on;
        m.SetChecks(c);
        for (int i = 0; i <= 10; ++i) {
            rig::Sample s;
            s.t = 3000.0 + i * 0.25;
            s.ok = true;
            s.farPct = 0.5f;
            s.scaleMeasured = true;
            s.scalePct = i < 8 ? (i == 4 ? -1.2f : 1.0f) : 0.1f;  // 2 s over the 0.5 % budget, then clear
            m.AddSample(s, "01:00:00:00");
        }
        const std::vector<rig::Alert> a = m.Alerts();
        std::printf("  scale alert (check %s): %s\n", on ? "on" : "off", a.empty() ? "(none)" : a.back().what.c_str());
        if (on) {
            CHECK(a.size() == 1 && a[0].end >= 0, "one closed scale alert");
            CHECK(!a.empty() && a[0].what.find("Scale mismatch -1.20% for 2 s (limit 0.50%)") == 0,
                  "the worst reading by size, with its sign");
        } else {
            CHECK(a.empty(), "qc.scale_check off: no scale alert");
        }
    }
}

// --- Colour (#270) ---------------------------------------------------------------------

std::vector<uint8_t> ToRgba(const livepix::Nv12& px);  // below, with the stills

rig::EyePair DownTest(const TestSignalConfig& cfg, int64_t frame = 30) {
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, frame, px);
    rig::EyePair e;
    rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e);
    return e;
}

void ColourGains() {
    // each channel's gain on its own, 0.97 / 1.00 / 1.03: that channel moves, the others not
    const char* names = "RGB";
    for (int c = 0; c < 3; ++c)
        for (float g : {0.97f, 1.00f, 1.03f}) {
            TestSignalConfig cfg;
            (c == 0 ? cfg.gainR : c == 1 ? cfg.gainG : cfg.gainB) = g;
            const rig::ColourMatch m = rig::MeasureColour(DownTest(cfg));
            std::printf("  gain %c %.2f: R %+.2f%% G %+.2f%% B %+.2f%% | lift R %+.2f G %+.2f B %+.2f | shape %.2f %.2f %.2f | Y %+.2f%%\n",
                        names[c], g, m.gainPct[0], m.gainPct[1], m.gainPct[2], m.liftPct[0], m.liftPct[1], m.liftPct[2],
                        m.shape[0], m.shape[1], m.shape[2], m.lumaPct);
            char msg[96];
            std::snprintf(msg, sizeof(msg), "gain %c %.2f recovered within +-0.3 %%", names[c], g);
            CHECK(m.measured && std::fabs(m.gainPct[c] - (g - 1.f) * 100.f) < 0.3f, msg);
            std::snprintf(msg, sizeof(msg), "gain %c %.2f: the other channels stay within +-0.3 %%", names[c], g);
            CHECK(std::fabs(m.gainPct[(c + 1) % 3]) < 0.3f && std::fabs(m.gainPct[(c + 2) % 3]) < 0.3f, msg);
            std::snprintf(msg, sizeof(msg), "gain %c %.2f: no lift read (+-0.3 %%)", names[c], g);
            CHECK(std::fabs(m.liftPct[0]) < 0.3f && std::fabs(m.liftPct[1]) < 0.3f && std::fabs(m.liftPct[2]) < 0.3f, msg);
        }
    // a lift on red: lift, not gain
    for (float l : {-0.02f, 0.02f}) {
        TestSignalConfig cfg;
        cfg.liftR = l;
        const rig::ColourMatch m = rig::MeasureColour(DownTest(cfg));
        std::printf("  lift R %+.2f: gain R %+.2f%% lift R %+.2f%% (G %+.2f B %+.2f)\n", l, m.gainPct[0], m.liftPct[0],
                    m.liftPct[1], m.liftPct[2]);
        CHECK(m.measured && std::fabs(m.liftPct[0] - l * 100.f) < 0.3f, "lift R recovered within +-0.3 % of full scale");
        CHECK(std::fabs(m.gainPct[0]) < 0.3f, "a lift is not read as a gain");
    }
    // matched eyes: dE (now over the overlap) stays small, the shape is ~1
    const rig::Result r = MeasureTest(TestSignalConfig{});
    std::printf("  matched: dE %.2f shape %.3f %.3f %.3f luma %+.2f%% (%+.3f stop)\n", r.deltaE, r.colour.shape[0],
                r.colour.shape[1], r.colour.shape[2], r.colour.lumaPct, r.colour.lumaStops);
    CHECK(r.ok && r.deltaE < 0.5f, "dE on matched eyes under 0.5");
    CHECK(r.colour.shape[0] > 0.9f && r.colour.shape[1] > 0.9f && r.colour.shape[2] > 0.9f, "matched: shape ~1");
}

// The live picture moves: single readings over 6 s of the moving scene, and their median
// (what RigMonitor shows, the median of the last 5).
void ColourOverFrames() {
    TestSignalConfig cfg;
    cfg.gainR = 1.03f;
    cfg.gainB = 0.98f;
    std::vector<float> r, b;
    for (int64_t f = 30; f <= 390; f += 30) {
        const rig::ColourMatch m = rig::MeasureColour(DownTest(cfg, f));
        r.push_back(m.gainPct[0]);
        b.push_back(m.gainPct[2]);
    }
    std::vector<float> rs = r, bs = b;
    std::sort(rs.begin(), rs.end());
    std::sort(bs.begin(), bs.end());
    std::printf("  over 13 frames: R %+.2f..%+.2f (median %+.2f)  B %+.2f..%+.2f (median %+.2f)\n", rs.front(), rs.back(),
                rs[rs.size() / 2], bs.front(), bs.back(), bs[bs.size() / 2]);
    CHECK(std::fabs(rs[rs.size() / 2] - 3.f) < 0.3f && std::fabs(bs[bs.size() / 2] + 2.f) < 0.3f,
          "moving scene: the median reading within +-0.3 %");
}

// A bright object only in the left eye's outer band (the left 6 % of the left eye, which
// the right eye does not see) must not move the gains: the overlap mask leaves it out.
void ColourOverlapMask() {
    TestSignalConfig cfg;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    rig::EyePair e;
    rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e);
    const rig::ColourMatch clean = rig::MeasureColour(e);
    const float dEClean = rig::DeltaE76(e.meanRgb[0], e.meanRgb[1]);
    const int w = cfg.eyeW, h = cfg.eyeH;
    for (int y = h * 3 / 10; y < h * 6 / 10; ++y)
        for (int x = w / 100; x < w * 6 / 100; ++x) px.y[(size_t)y * px.width + x] = 230;  // white, left eye only
    rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e);
    const rig::ColourMatch edge = rig::MeasureColour(e);
    const float dEEdge = rig::DeltaE76(e.meanRgb[0], e.meanRgb[1]);
    std::printf("  left-band object: gain R %+.3f/%+.3f G %+.3f/%+.3f B %+.3f/%+.3f %% dE %.3f/%.3f\n", clean.gainPct[0],
                edge.gainPct[0], clean.gainPct[1], edge.gainPct[1], clean.gainPct[2], edge.gainPct[2], dEClean, dEEdge);
    bool same = true;
    for (int c = 0; c < 3; ++c)
        same = same && std::fabs(edge.gainPct[c] - clean.gainPct[c]) < 0.05f && std::fabs(edge.liftPct[c] - clean.liftPct[c]) < 0.05f;
    CHECK(edge.measured && same, "an object in the left eye's outer band leaves the gains alone");
    CHECK(std::fabs(dEEdge - dEClean) < 0.05f, "... and the dE");
}

// Matte rows (a letterbox) count in neither eye.
void ColourLetterbox() {
    TestSignalConfig cfg;
    cfg.gainB = 1.03f;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    for (int y = 0; y < px.height; ++y)
        if (y < px.height / 8 || y >= px.height * 7 / 8) {
            std::fill(px.y.begin() + (size_t)y * px.width, px.y.begin() + (size_t)(y + 1) * px.width, (uint8_t)16);
            if (y % 2 == 0)
                std::fill(px.uv.begin() + (size_t)(y / 2) * px.width, px.uv.begin() + (size_t)(y / 2 + 1) * px.width, (uint8_t)128);
        }
    rig::EyePair e;
    rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e);
    const rig::ColourMatch m = rig::MeasureColour(e);
    std::printf("  letterboxed, gain B 1.03: B %+.2f%% lift B %+.2f%% (bins 0: %u / %u of %lld)\n", m.gainPct[2], m.liftPct[2],
                e.hist[0][3][0], e.hist[1][3][0], (long long)e.colourSamples[0]);
    CHECK(m.measured && std::fabs(m.gainPct[2] - 3.f) < 0.3f, "letterboxed: the gain still reads");
    CHECK(e.hist[0][3][0] < (uint32_t)(e.colourSamples[0] / 100), "letterbox rows are not in the histogram");
}

// The .cc the panel copies, applied to the right eye as a grading tool would (ASC CDL on the
// code values), brings the gains under 0.5 %.
void ColourFixRoundTrip() {
    TestSignalConfig cfg;
    cfg.gainR = 1.03f;
    cfg.gainG = 0.98f;
    cfg.gainB = 1.02f;
    cfg.liftR = 0.01f;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    std::vector<uint8_t> rgba = ToRgba(px);
    rig::EyePair e;
    rig::DownsampleRgba(rgba.data(), px.width, px.height, StereoLayout::SbsFull, 640, e);
    const rig::ColourMatch before = rig::MeasureColour(e);
    const std::string cc = rig::ColourFixCc(before);
    color::Cdl cdl;
    std::string err;
    CHECK(color::ParseCdl(cc, cdl, err), "the .cc parses as an ASC CDL");
    for (int y = 0; y < px.height; ++y)
        for (int x = px.width / 2; x < px.width; ++x) {  // the right eye only
            uint8_t* p = &rgba[((size_t)y * px.width + x) * 4];
            for (int k = 0; k < 3; ++k) {
                const float v = std::clamp(p[k] / 255.f * cdl.slope[k] + cdl.offset[k], 0.f, 1.f);
                p[k] = (uint8_t)std::lround(std::pow(v, cdl.power[k]) * 255.f);
            }
        }
    rig::DownsampleRgba(rgba.data(), px.width, px.height, StereoLayout::SbsFull, 640, e);
    const rig::ColourMatch after = rig::MeasureColour(e);
    std::printf("  .cc fix: before R %+.2f G %+.2f B %+.2f %% (lift R %+.2f) -> after R %+.2f G %+.2f B %+.2f %%\n",
                before.gainPct[0], before.gainPct[1], before.gainPct[2], before.liftPct[0], after.gainPct[0],
                after.gainPct[1], after.gainPct[2]);
    CHECK(before.measured && std::fabs(before.gainPct[1] + 2.f) < 0.4f, "still: the gains read before the fix");
    CHECK(after.measured && std::fabs(after.gainPct[0]) < 0.5f && std::fabs(after.gainPct[1]) < 0.5f &&
              std::fabs(after.gainPct[2]) < 0.5f,
          "after the .cc: every gain under 0.5 %");
}

// The colour-difference overlay: right minus left per cell, the right eye sampled at the
// median parallax. A brighter right eye reads positive luma everywhere; matched eyes ~0.
void ColourDiffMap() {
    auto stats = [](const rig::ColourDiff& d, float& med, int& n) {
        std::vector<float> v;
        for (float x : d.y)
            if (!std::isnan(x)) v.push_back(x);
        n = (int)v.size();
        if (v.empty()) return;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        med = v[v.size() / 2];
    };
    float m0 = 0, m1 = 0;
    int n0 = 0, n1 = 0;
    const rig::Result clean = MeasureTest(TestSignalConfig{});
    stats(clean.colourDiff, m0, n0);
    TestSignalConfig hot;
    hot.gainG = 1.05f;  // G carries 72 % of luma
    const rig::Result r = MeasureTest(hot);
    stats(r.colourDiff, m1, n1);
    std::printf("  colour diff map: %dx%d, clean median %+.4f (%d cells), G x1.05 median %+.4f (%d cells)\n",
                r.colourDiff.cols, r.colourDiff.rows, m0, n0, m1, n1);
    CHECK(clean.colourDiff.Ok() && n0 > clean.colourDiff.cols * clean.colourDiff.rows / 2, "the map covers the overlap");
    CHECK(std::fabs(m0) < 0.003f, "matched eyes: the map is ~0");
    CHECK(m1 > 0.005f, "a brighter right eye reads positive");
}

void ColourAlert() {
    rig::RigMonitor m;
    for (int i = 0; i <= 10; ++i) {
        rig::Sample s;
        s.t = 4000.0 + i * 0.25;
        s.ok = true;
        s.farPct = 0.5f;
        s.colourMeasured = true;
        s.gainPct[0] = 0.4f;
        s.gainPct[2] = i < 8 ? (i == 3 ? -3.4f : -2.6f) : -0.5f;  // blue 2 s past the 2 % budget
        m.AddSample(s, "01:00:00:00");
    }
    const std::vector<rig::Alert> a = m.Alerts();
    std::printf("  colour gain alert: %s\n", a.empty() ? "(none)" : a.back().what.c_str());
    CHECK(a.size() == 1 && a[0].what.find("Colour gain B -3.4% for 2 s (limit 2.0%)") == 0, "gain alert names the channel");
}

// --- Stills and top/bottom (#266) ------------------------------------------------------

// Limited-range BT.709 NV12 -> 8-bit R'G'B'A, what a decoded side-by-side photo hands OnStill.
std::vector<uint8_t> ToRgba(const livepix::Nv12& px) {
    std::vector<uint8_t> o((size_t)px.width * px.height * 4);
    for (int y = 0; y < px.height; ++y)
        for (int x = 0; x < px.width; ++x) {
            const float Y = (px.y[(size_t)y * px.width + x] - 16.f) / 219.f;
            const size_t c = (size_t)(y / 2) * px.width + (x & ~1);
            const float u = (px.uv[c] - 128.f) / 224.f, v = (px.uv[c + 1] - 128.f) / 224.f;
            const float rgb[3] = {Y + 1.5748f * v, Y - 0.1873f * u - 0.4681f * v, Y + 1.8556f * u};
            uint8_t* d = &o[((size_t)y * px.width + x) * 4];
            for (int k = 0; k < 3; ++k) d[k] = (uint8_t)std::lround(std::clamp(rgb[k], 0.f, 1.f) * 255.f);
            d[3] = 255;
        }
    return o;
}

// Restack an SBS-full frame as top/bottom: full (each eye at its size, the frame twice as
// tall) or half (each eye squeezed to half its height, every 2nd row).
livepix::Nv12 ToTopBottom(const livepix::Nv12& px, bool half) {
    const int ew = px.width / 2, eh = px.height, step = half ? 2 : 1, oh = eh / step;
    livepix::Nv12 o;
    o.Resize(ew, 2 * oh);
    for (int eye = 0; eye < 2; ++eye) {
        for (int y = 0; y < oh; ++y)
            std::copy_n(&px.y[(size_t)(y * step) * px.width + eye * ew], ew, &o.y[(size_t)(eye * oh + y) * ew]);
        for (int y = 0; y < oh / 2; ++y)
            std::copy_n(&px.uv[(size_t)(y * step) * px.width + eye * ew], ew, &o.uv[(size_t)(eye * oh / 2 + y) * ew]);
    }
    return o;
}

void PrintResult(const char* what, const rig::Result& r) {
    std::printf("  %-22s ok=%d matches=%3d vertical=%5.2f px roll=%5.2f near=%+.2f%% far=%+.2f%% dE=%.2f edges %d/%d\n",
                what, r.ok, r.matches, r.verticalPx, r.rollDeg, r.nearPct, r.farPct, r.deltaE, r.edge[0].measured,
                r.edge[1].measured);
}

// The test plate as a side-by-side still, top/bottom (full and half, video and still) and
// swapped reads what the side-by-side video frame reads, within the matcher's noise.
void StillsAndTopBottom() {
    TestSignalConfig cfg;
    cfg.vShiftPx = 3.0f;
    cfg.rollDeg = 0.4f;
    cfg.gainR = 1.15f;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    rig::EyePair sbs, e;
    CHECK(rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, sbs), "SBS downsamples");
    const rig::Result ref = rig::Measure(sbs);
    PrintResult("SBS video frame", ref);
    auto same = [&](const char* what, const rig::Result& r, float dETol) {
        PrintResult(what, r);
        char m[128];
        std::snprintf(m, sizeof(m), "%s: measured", what);
        CHECK(r.ok, m);
        std::snprintf(m, sizeof(m), "%s: vertical error as SBS", what);
        CHECK(std::fabs(r.verticalPx - ref.verticalPx) < 0.4f, m);
        std::snprintf(m, sizeof(m), "%s: roll as SBS", what);
        CHECK(std::fabs(r.rollDeg - ref.rollDeg) < 0.1f, m);
        std::snprintf(m, sizeof(m), "%s: depth range as SBS", what);
        CHECK(std::fabs(r.nearPct - ref.nearPct) < 0.2f && std::fabs(r.farPct - ref.farPct) < 0.2f, m);
        std::snprintf(m, sizeof(m), "%s: colour match as SBS", what);
        CHECK(std::fabs(r.deltaE - ref.deltaE) < dETol, m);
        std::snprintf(m, sizeof(m), "%s: frame edges as SBS", what);
        CHECK(r.edge[0].measured == ref.edge[0].measured && r.edge[1].measured == ref.edge[1].measured, m);
    };
    // TB full carries the same pixels per eye: the analysis image is the same, bit for bit
    CHECK(rig::Downsample(ToFrame(ToTopBottom(px, false)), StereoLayout::TbFull, 640, e), "TB-full downsamples");
    CHECK(e.left == sbs.left && e.right == sbs.right && e.meanRgb[1][0] == sbs.meanRgb[1][0],
          "TB-full analysis image = SBS's");
    same("TB-full video frame", rig::Measure(e), 0.01f);
    CHECK(rig::Downsample(ToFrame(ToTopBottom(px, true)), StereoLayout::TbHalf, 640, e), "TB-half downsamples");
    CHECK(e.yStretch == 2.f && e.h == sbs.h, "TB-half: displayed rows restored, stretch 2");
    same("TB-half video frame", rig::Measure(e), 0.3f);
    // stills: the eyes from R'G'B' (dE is on R'G'B' means instead of Y'CbCr ones: a little apart)
    const std::vector<uint8_t> rgba = ToRgba(px);
    CHECK(rig::DownsampleRgba(rgba.data(), px.width, px.height, StereoLayout::SbsFull, 640, e), "RGBA still downsamples");
    same("SBS still", rig::Measure(e), 0.5f);
    const livepix::Nv12 tb = ToTopBottom(px, false);
    const std::vector<uint8_t> tbRgba = ToRgba(tb);
    CHECK(rig::DownsampleRgba(tbRgba.data(), tb.width, tb.height, StereoLayout::TbFull, 640, e), "TB still downsamples");
    same("TB-full still", rig::Measure(e), 0.5f);
    // an R|L still with the swap flag reads as the L|R one
    const livepix::Nv12 sw = SwapHalves(px);
    const std::vector<uint8_t> swRgba = ToRgba(sw);
    CHECK(rig::DownsampleRgba(swRgba.data(), sw.width, sw.height, StereoLayout::SbsFull, 640, e, true), "swapped still");
    same("R|L still, swapped", rig::Measure(e), 0.5f);
    CHECK(!rig::DownsampleRgba(rgba.data(), px.width, px.height, StereoLayout::Mono, 640, e), "a mono still is not measured");
    // half SBS still: the roll on screen, not 2x
    const livepix::Nv12 hs = Squeeze(px);
    const std::vector<uint8_t> hsRgba = ToRgba(hs);
    CHECK(rig::DownsampleRgba(hsRgba.data(), hs.width, hs.height, StereoLayout::SbsHalf, 640, e), "half-SBS still");
    const rig::Result h = rig::Measure(e);
    PrintResult("half-SBS still", h);
    CHECK(h.ok && std::fabs(h.rollDeg - ref.rollDeg) < 0.15f && std::fabs(h.verticalPx - ref.verticalPx) < 0.5f,
          "half-SBS still: displayed roll and vertical error");
}

// The monitor's still path: measured once without frames, held while shown, re-measured
// on an eye-swap / layout change, ended by frames; a GPU-only frame says why it is not
// measured.
void MonitorStill() {
    TestSignalConfig cfg;
    cfg.vShiftPx = 4.0f;
    cfg.eyeW = 2880;  // a 5760x1620 still: kept reduced (k = 3), readings still in source px
    cfg.eyeH = 1620;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    const std::vector<uint8_t> rgba = ToRgba(px);
    rig::RigMonitor m;
    auto waitSamples = [&](size_t n) {
        for (int i = 0; i < 500 && m.History(1e9).size() < n; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return m.History(1e9).size() >= n;
    };
    m.SubmitStill(rgba.data(), px.width, px.height, StereoLayout::SbsFull, false);
    CHECK(waitSamples(1), "a still is measured without any frames");
    rig::Result r = m.Last();
    PrintResult("monitor still", r);
    CHECK(r.ok && std::fabs(r.verticalPx - 4.0f) < 0.75f, "still: 4 px vertical error, in source px");
    CHECK(r.eyeSyncMeasuredFrames == -1, "still: no eye sync");
    CHECK(m.StillShown() && m.Active(LiveProducer::NowSeconds() + 10.0), "still: held long after it was measured");
    CHECK(m.Alerts().empty(), "still: no alert (no duration)");
    m.UpdateStill(StereoLayout::SbsFull, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(m.History(1e9).size() == 1, "no change, no re-measure");
    m.UpdateStill(StereoLayout::SbsFull, true);
    CHECK(waitSamples(2), "eye swap re-measures");
    r = m.Last();
    PrintResult("monitor still, swapped", r);
    CHECK(r.ok && r.verticalPx < -3.0f, "swapped: vertical sign follows the eyes as shown");
    m.UpdateStill(StereoLayout::Mono, false);
    CHECK(waitSamples(3), "layout change re-measures");
    CHECK(!m.Last().ok, "mono: not measured");
    // a zero-copy (GPU-only) frame ends the still and says why it cannot be measured
    FrameRing::Frame g;
    g.width = 3840;
    g.height = 1080;
    g.format = PixFormat::RGBA;
    g.gpu = true;
    m.Submit(g, StereoLayout::SbsFull, -1, "");
    CHECK(!m.StillShown(), "frames end the still");
    CHECK(m.Unmeasurable() != nullptr && std::string(m.Unmeasurable()).find("CPU frames") != std::string::npos,
          "GPU-only frames: the reason is given");
    m.Submit(ToFrame(px), StereoLayout::SbsFull, -1, "");
    CHECK(m.Unmeasurable() == nullptr, "CPU frames: no reason");
}

void TooFewMatches() {
    // flat grey except one small textured patch per eye (9 px cells, shifted 9 px): a
    // handful of matches, not enough to call the rig measured
    livepix::Nv12 px;
    px.Resize(3840, 1080);
    std::fill(px.y.begin(), px.y.end(), (uint8_t)90);
    std::fill(px.uv.begin(), px.uv.end(), (uint8_t)128);
    uint32_t seed = 1234;
    for (int y = 400; y < 560; ++y)
        for (int x = 800; x < 960; ++x) {
            uint32_t hsh = (uint32_t)((x / 9) * 73856093) ^ (uint32_t)((y / 9) * 19349663) ^ seed;
            hsh = (hsh ^ (hsh >> 13)) * 0x5bd1e995u;
            const uint8_t v = (uint8_t)(40 + ((hsh >> 24) % 160));
            px.y[(size_t)y * px.width + x] = v;
            px.y[(size_t)y * px.width + 1920 + x + 9] = v;
        }
    rig::EyePair e;
    rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e);
    const rig::Result r = rig::Measure(e);
    std::printf("  small patch: ok=%d matches=%d\n", r.ok, r.matches);
    CHECK(r.matches > 0, "the patch does match");
    CHECK(!r.ok, "a handful of matches is not a measurement");
}

// Edge (window) violations. A synthetic pair: a textured background at the screen plane
// (identical in both eyes) and a textured box of its own, at x0..x0+w of the left eye and
// shifted by parallaxPx in the right (right minus left; negative = in front of the screen).
float Cell(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)((x >> 2) * 73856093) ^ (uint32_t)((y >> 2) * 19349663) ^ seed;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    return (float)(40 + ((h >> 24) % 170));
}

rig::EyePair EdgeScene(int x0, int w, int parallaxPx) {
    rig::EyePair e;
    e.eyeW = 640;
    e.h = 360;
    e.fullEyeW = 640;
    e.fullH = 360;
    e.left.resize((size_t)e.eyeW * e.h);
    e.right.resize(e.left.size());
    const int y0 = 110, y1 = 250;
    for (int y = 0; y < e.h; ++y)
        for (int x = 0; x < e.eyeW; ++x) {
            const bool inY = y >= y0 && y < y1;
            const int lx = x - x0, rx = x - x0 - parallaxPx;  // box-local x in each eye
            e.left[(size_t)y * e.eyeW + x] = inY && lx >= 0 && lx < w ? Cell(lx, y, 777u) : Cell(x, y, 31u);
            e.right[(size_t)y * e.eyeW + x] = inY && rx >= 0 && rx < w ? Cell(rx, y, 777u) : Cell(x, y, 31u);
        }
    for (int i = 0; i < 2; ++i)
        for (int c = 0; c < 3; ++c) e.meanRgb[i][c] = 0.5f;
    return e;
}

void EdgeViolations() {
    const rig::Budget b;
    const float px2pct = 100.f / 640.f;
    // a box in front (-13 px = -2.0 %) cut by the left edge
    rig::Result r = rig::Measure(EdgeScene(-20, 140, -13));
    std::printf("  left edge, in front: L measured=%d %.2f%% (%zu)  R measured=%d %.2f%%\n", r.edge[0].measured,
                r.edge[0].parallaxPct, r.edge[0].parallax.size(), r.edge[1].measured, r.edge[1].parallaxPct);
    CHECK(r.edge[0].measured && std::fabs(r.edge[0].parallaxPct + 13 * px2pct) < 0.3f, "left edge reads the box's -2%");
    CHECK(r.edge[0].measured && r.edge[0].parallaxPct < b.edgePct, "left edge violation flagged");
    CHECK(r.edge[1].measured && r.edge[1].parallaxPct >= b.edgePct, "right edge clean");
    // the same at the right edge
    r = rig::Measure(EdgeScene(640 - 120, 140, -13));
    std::printf("  right edge, in front: L %.2f%%  R measured=%d %.2f%%\n", r.edge[0].parallaxPct, r.edge[1].measured,
                r.edge[1].parallaxPct);
    CHECK(r.edge[1].measured && std::fabs(r.edge[1].parallaxPct + 13 * px2pct) < 0.3f, "right edge reads the box's -2%");
    CHECK(r.edge[0].measured && r.edge[0].parallaxPct >= b.edgePct, "left edge clean");
    // behind the screen at the edge is fine (the window is in front of it)
    r = rig::Measure(EdgeScene(-20, 140, 7));
    std::printf("  left edge, behind: L measured=%d %.2f%%\n", r.edge[0].measured, r.edge[0].parallaxPct);
    CHECK(!(r.edge[0].measured && r.edge[0].parallaxPct < b.edgePct), "behind the screen at the edge is not a violation");
    // in front but in the middle of the frame: depth, not an edge violation
    r = rig::Measure(EdgeScene(250, 140, -13));
    std::printf("  centre, in front: near %.2f%%  L %.2f%%  R %.2f%%\n", r.nearPct, r.edge[0].parallaxPct,
                r.edge[1].parallaxPct);
    CHECK(r.ok && r.nearPct < -1.5f, "the centre box is seen as near");
    CHECK(r.edge[0].measured && r.edge[1].measured, "both edges measured on a textured background");
    CHECK(r.edge[0].parallaxPct >= b.edgePct && r.edge[1].parallaxPct >= b.edgePct, "no edge violation");
}

void EdgeAlert() {
    rig::RigMonitor m;
    for (int i = 0; i <= 10; ++i) {
        rig::Sample s;
        s.t = 2000.0 + i * 0.25;
        s.ok = true;
        s.farPct = 0.5f;
        s.edgeMeasured[0] = s.edgeMeasured[1] = true;
        s.edgePct[0] = i < 8 ? -1.5f : 0.0f;  // 2 s cut off by the left edge, then clear
        s.edgePct[1] = 0.0f;
        m.AddSample(s, "01:00:00:00");
    }
    const std::vector<rig::Alert> a = m.Alerts();
    std::printf("  edge alert: %s\n", a.empty() ? "(none)" : a.back().what.c_str());
    CHECK(a.size() == 1 && a[0].end >= 0, "one closed edge alert");
    CHECK(!a.empty() && a[0].what.find("Left edge violation at -1.5%") == 0, "named by side, with the worst parallax");
}

// #135: the divergence alert and the target figures are for the picture as shown, so
// convergence counts; the source's near / far alerts do not change with it.
void DivergenceFollowsConvergence() {
    rig::Budget b;
    b.farPct = 5.0f;
    b.divergeFarPct = 1.7f;
    b.targetName = "Cinema";
    b.targetWidthM = 10.0f;
    b.targetDistanceM = 12.0f;
    auto run = [&](float offsetPct) {
        rig::RigMonitor m;
        m.SetBudget(b);
        m.SetTargetParallaxOffset(offsetPct);
        for (int i = 0; i <= 10; ++i) {
            rig::Sample s;
            s.t = 3000.0 + i * 0.25;
            s.ok = true;
            s.farPct = 1.2f;
            m.AddSample(s, "01:00:00:00");
        }
        return m.Alerts();
    };
    CHECK(run(0.0f).empty(), "+1.2% stays under the cinema's divergence limit");
    CHECK(run(-1.0f).empty(), "convergence bringing the background forward does not alert");
    const std::vector<rig::Alert> a = run(1.0f);
    std::printf("  divergence alert: %s\n", a.empty() ? "(none)" : a.back().what.c_str());
    CHECK(a.size() == 1, "convergence pushing it to +2.2% alerts");
    CHECK(!a.empty() && a[0].what.find("Background diverges on the target screen at +2.2%") == 0, "quotes the parallax as shown");
    // 220 mm on 10 m: 2 atan((0.220 - 0.065) / 24) = 0.74 deg, counted once, not twice.
    CHECK(!a.empty() && a[0].what.find("[0.74 deg on Cinema]") != std::string::npos, "the angle includes the offset once");
}

void AlertClosesOnBlack() {
    rig::RigMonitor m;
    rig::Budget b;
    b.maxVerticalPx = 1.0f;
    m.SetBudget(b);
    auto sample = [](double t, bool ok, float v) {
        rig::Sample s;
        s.t = t;
        s.ok = ok;
        s.verticalPx = v;
        s.deltaE = 0.5f;
        s.nearPct = -0.5f;
        s.farPct = 0.5f;
        return s;
    };
    // 1 s of a 3 px vertical error, then the picture cuts to black (nothing to measure)
    for (int i = 0; i <= 4; ++i) {
        char tc[16];
        std::snprintf(tc, sizeof(tc), "01:00:00:%02d", i * 6);
        m.AddSample(sample(1000.0 + i * 0.25, true, 3.0f), tc);
    }
    std::vector<rig::Alert> a = m.Alerts();
    CHECK(a.size() == 1 && a[0].end < 0, "an open alert while the error lasts");
    for (int i = 1; i <= 12; ++i) m.AddSample(sample(1001.0 + i * 0.25, false, 0.f), "");
    a = m.Alerts();
    std::printf("  cut to black: %zu alert(s)%s%s\n", a.size(), a.empty() ? "" : ", ",
                a.empty() ? "" : (a[0].what + " end TC " + a[0].endTimecode).c_str());
    CHECK(a.size() == 1 && a[0].end >= 0, "the alert closed after the cut");
    CHECK(!a.empty() && std::fabs(a[0].end - 1001.0) < 1e-6, "closed at the last good sample");
    CHECK(!a.empty() && a[0].timecode == "01:00:00:00" && a[0].endTimecode == "01:00:00:24",
          "start and last-good timecodes");
}

// The render thread's path (RigMonitor::Submit) in real time, with frames dropping:
// UHD 59.94 on the station dropped a frame every few, the burst restarted its 0.5 s
// fallback window at every gap, and the rig check measured nothing (went blank). Drops
// must still give a measurement about every 0.5 s; a clean feed keeps its ~4 a second.
size_t MeasurementsWhileFeeding(int dropEveryN, double seconds) {
    TestSignalConfig cfg;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    FrameRing::Frame f = ToFrame(px);
    rig::RigMonitor m;
    int64_t serial = 0;
    int frames = 0;
    const double t0 = LiveProducer::NowSeconds();
    while (LiveProducer::NowSeconds() - t0 < seconds) {
        ++frames;
        serial += (dropEveryN > 0 && frames % dropEveryN == 0) ? 2 : 1;  // skip one serial = a dropped frame
        f.serial = serial;
        m.Submit(f, StereoLayout::SbsFull, -1, "01:00:00:00");
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    for (int i = 0; i < 200 && m.LastMeasureMs() <= 0.0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the last hand-over finish
    return m.History(seconds + 5.0).size();
}

void MeasuresWithDroppedFrames() {
    const size_t clean = MeasurementsWhileFeeding(0, 2.0);
    const size_t drops3 = MeasurementsWhileFeeding(3, 2.0);
    const size_t drops2 = MeasurementsWhileFeeding(2, 2.0);
    std::printf("  measurements in 2 s: clean %zu, a frame dropped every 3rd %zu, every 2nd %zu\n", clean, drops3,
                drops2);
    // ~4 a second nominally (8 in 2 s); a slow CI runner (macOS) has measured 4, so only
    // check that a clean feed measures more often than the drop cases' ~2 a second.
    CHECK(clean >= 3, "a clean feed is measured several times a second");
    CHECK(drops3 >= 2, "dropping every 3rd frame still measures (about every 0.5 s)");
    CHECK(drops2 >= 2, "dropping every 2nd frame still measures");
}

std::vector<rig::EyePair> Burst(const TestSignalConfig& cfg, int64_t last, bool swap) {
    std::vector<rig::EyePair> seq;
    for (int64_t n = last - rig::RigMonitor::kSyncFrames + 1; n <= last; ++n) {
        livepix::Nv12 px;
        TestSignalSource::Render(cfg, n, px);
        rig::EyePair e;
        rig::Downsample(ToFrame(swap ? SwapHalves(px) : px), StereoLayout::SbsFull, 640, e, false);
        seq.push_back(std::move(e));
    }
    return seq;
}

void EyeSyncMeasured() {
    TestSignalConfig cfg;
    cfg.fps = 24.0;  // a frame of motion a single burst can see; 48p/60p need the Worker's confirmation (below)
    int late = 0;
    const int d0 = rig::MeasureEyeSync(Burst(cfg, 30, false), 0, 0, 0, &late);
    std::printf("  eye delay 0: measured %d (late %d)\n", d0, late);
    CHECK(d0 == 0, "in step measures 0");
    cfg.eyeDelay = 2;
    const int d2 = rig::MeasureEyeSync(Burst(cfg, 30, false), 0, 0, 0, &late);
    std::printf("  eye delay 2: measured %d (late %d)\n", d2, late);
    CHECK(d2 == 2 && late == +1, "right eye 2 frames late measured from the pixels");
    // the same rig seen with the halves swapped: now the LEFT eye is the late one
    const int dl = rig::MeasureEyeSync(Burst(cfg, 30, true), 0, 0, 0, &late);
    std::printf("  eye delay 2, halves swapped: measured %d (late %d)\n", dl, late);
    CHECK(dl == 2 && late == -1, "left eye late measured");
    // a still picture has no motion: not measured
    std::vector<rig::EyePair> still(rig::RigMonitor::kSyncFrames, Burst(cfg, 30, false).back());
    CHECK(rig::MeasureEyeSync(still, 0, 0, 0, &late) == -1, "no motion = not measured");
}


// The app's path (RigMonitor::Worker): the rig's measured parallax and vertical offset
// are removed before the eyes are compared, at the station's live config: 48p, a 4 px
// vertical shift, a brighter right eye, a 2-frame-late right eye. Reported from the
// Windows box as "measured 0, timecode says 2" (tmp-mp-pro, 2026-10-18).
void EyeSyncLikeTheApp(float rollDeg, int& confirmedRightTotal) {
    TestSignalConfig cfg;
    cfg.fps = 48.0;
    cfg.vShiftPx = 4.0f;
    cfg.gainR = 1.15f;
    cfg.eyeDelay = 2;
    cfg.rollDeg = rollDeg;
    std::printf("  app path, roll %.1f deg:\n", rollDeg);
    int wrong = 0, measured = 0, confirmedWrong = 0, confirmedRight = 0;
    rig::SyncConfirm confirm;
    // bursts every 20 frames over 6 s (the Worker manages 2-4 a second)
    std::vector<int64_t> lasts;
    for (int64_t f = 30; f <= 290; f += 20) lasts.push_back(f);
    for (int64_t last : lasts) {
        std::vector<rig::EyePair> seq = Burst(cfg, last, false);
        rig::Result r = rig::Measure(seq.back());
        CHECK(r.ok, "the spatial measurement works at the app's config");
        std::vector<float> p = r.parallaxPct;
        std::nth_element(p.begin(), p.begin() + p.size() / 2, p.end());
        const rig::EyePair& e = seq.back();
        const float shiftX = p[p.size() / 2] / 100.f * (float)e.eyeW;
        const float shiftY = e.fullH > 0 ? r.verticalPx * (float)e.h / e.fullH : 0.f;
        int late = 0;
        const int d = rig::MeasureEyeSync(seq, shiftX, shiftY, r.dySlope, &late);
        std::printf("  app path, frame %lld: shift %.2f,%.2f vertical %.2f px -> measured %d (late %d)\n",
                    (long long)last, shiftX, shiftY, r.verticalPx, d, late);
        if (d >= 0) ++measured;
        if (d >= 0 && !(d == 2 && late == +1)) ++wrong;
        int cl = 0;
        const int cd = confirm.Feed(d, late, (double)last / cfg.fps, &cl);  // what the Worker would show
        if (cd >= 0) ((cd == 2 && cl == +1) ? confirmedRight : confirmedWrong)++;
        // the Worker's follow-up: the rig's geometry on the time-aligned pair (the right
        // eye now against the left eye d frames back) reads the injected 4 px, not the
        // moving discs seen through the delay
        if (d == 2 && late == +1) {
            rig::EyePair aligned = seq.back();
            aligned.left = seq[seq.size() - 1 - 2].left;
            const rig::Result a = rig::Measure(aligned);
            std::printf("    aligned pair: vertical %.2f px\n", a.verticalPx);
            CHECK(a.ok && std::fabs(a.verticalPx - 4.0f) < 0.6f, "vertical error on the aligned pair = 4 px");
            CHECK(a.ok && a.farPct < 3.0f, "no search-limit outlier in the far depth");
        }
    }
    std::printf("    single bursts: %d measured, %d wrong; confirmed: %d right, %d wrong\n", measured, wrong,
                confirmedRight, confirmedWrong);
    CHECK(confirmedWrong == 0, "never a confirmed wrong eye-sync reading on the app's path");
    CHECK(wrong <= 2, "at most two wrong single-burst readings in fourteen");
    // was >= 3 on the aliased analysis image (2x2 of every 3x3 source pixels)
    CHECK(measured >= 5, "the delay is read from the pixels in a fair share of bursts");
    CHECK(confirmedRight >= 1, "the 2-frame delay is shown from the pixels at this roll");
    confirmedRightTotal += confirmedRight;
}

// The vote that turns single-burst readings into the figure shown (RigMonitor::Worker).
void SyncConfirmVotes() {
    rig::SyncConfirm c;
    int late = 0;
    double t = 0;
    auto feed = [&](int d, int l) { return c.Feed(d, l, t += 0.4, &late); };
    CHECK(feed(2, +1) == -1 && feed(-1, 0) == -1 && feed(2, +1) == -1, "two votes are not enough");
    CHECK(feed(2, +1) == 2 && late == +1, "three votes show the reading");
    CHECK(feed(-1, 0) == 2 && feed(-1, 0) == 2 && late == +1, "held between readings (bursts that read nothing)");
    CHECK(feed(1, +1) == 2 && feed(1, +1) == -1, "a neighbour that gains half the votes blanks it");
    rig::SyncConfirm d;
    t = 0;
    for (int i = 0; i < 3; ++i) d.Feed(2, +1, t += 0.4, &late);
    CHECK(d.Feed(-1, 0, t + rig::SyncConfirm::kWindowSec + 1.0, &late) == -1, "readings expire after the window");
}

}  // namespace

// The luma box filter as it was before it ran threaded with integer sums (float sums, one
// thread): the threaded integer version must equal it bit for bit.
std::vector<float> ReferenceLuma(const FrameRing::Frame& f, int analysisW, bool rightEye, int& aw, int& ah) {
    const bool p16 = f.format == PixFormat::P016;
    const int fullEye = f.width / 2;
    aw = std::min(analysisW, fullEye);
    const float sx = (float)fullEye / (float)aw;
    ah = std::max(16, (int)std::lround(f.height / sx));
    const float sy = (float)f.height / (float)ah;
    const int W = f.width, x0 = rightEye ? fullEye : 0;
    std::vector<float> dst((size_t)aw * ah), colSum((size_t)fullEye);
    for (int y = 0; y < ah; ++y) {
        const int ya = std::min(f.height - 1, (int)(y * sy));
        const int yb = std::max(ya + 1, std::min(f.height, (int)((y + 1) * sy)));
        std::fill(colSum.begin(), colSum.end(), 0.f);
        for (int yy = ya; yy < yb; ++yy) {
            const size_t r = (size_t)yy * W + x0;
            if (p16) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(f.plane[0].data()) + r;
                for (int x = 0; x < fullEye; ++x) colSum[x] += (float)src[x] * (1.0f / 256.0f);
            } else {
                const uint8_t* src = f.plane[0].data() + r;
                for (int x = 0; x < fullEye; ++x) colSum[x] += (float)src[x];
            }
        }
        float* d = dst.data() + (size_t)y * aw;
        for (int x = 0; x < aw; ++x) {
            const int xa = std::min(fullEye - 1, (int)(x * sx));
            const int xb = std::max(xa + 1, std::min(fullEye, (int)((x + 1) * sx)));
            float acc = 0.f;
            for (int xx = xa; xx < xb; ++xx) acc += colSum[xx];
            d[x] = acc / (float)((xb - xa) * (yb - ya));
        }
    }
    return dst;
}

// The downsample runs on several threads; every analysis pixel and both mean colours must
// be bit-identical to the one-thread result (each output row is independent, each mean is
// summed on one thread). Odd sizes so the chunks do not divide evenly.
void ThreadedDownsampleIsExact() {
    TestSignalConfig cfg;
    cfg.eyeW = 965;  // a 1930x1086 frame
    cfg.eyeH = 1086;
    cfg.vShiftPx = 3.0f;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 17, px);
    const FrameRing::Frame nv12 = ToFrame(px);
    // The same picture as P016 (10/16-bit capture), MSB-aligned with a low byte set.
    FrameRing::Frame p016 = nv12;
    p016.format = PixFormat::P016;
    for (int pl = 0; pl < 2; ++pl) {
        const std::vector<uint8_t>& src = nv12.plane[pl];
        std::vector<uint8_t> wide(src.size() * 2);
        for (size_t i = 0; i < src.size(); ++i) {
            wide[i * 2] = (uint8_t)(i * 37);  // low byte
            wide[i * 2 + 1] = src[i];         // high byte
        }
        p016.plane[pl] = std::move(wide);
    }
    const FrameRing::Frame* frame = &nv12;
    auto run = [&](const char* threads, StereoLayout layout, bool swap) {
        const FrameRing::Frame& f = *frame;
#if defined(_WIN32)
        _putenv_s("MEDIAPLAYER_RIG_THREADS", threads);
#else
        setenv("MEDIAPLAYER_RIG_THREADS", threads, 1);
#endif
        rig::EyePair e;
        CHECK(rig::Downsample(f, layout, 333, e, swap), "downsample");
        return e;
    };
    for (int fmt = 0; fmt < 2; ++fmt)
    for (int swap = 0; swap < 2; ++swap) {
        frame = fmt == 0 ? &nv12 : &p016;
        const rig::EyePair one = run("1", StereoLayout::SbsFull, swap != 0);
        int raw = 0, rah = 0;
        const std::vector<float> refL = ReferenceLuma(*frame, 333, swap != 0, raw, rah);
        const std::vector<float> refR = ReferenceLuma(*frame, 333, swap == 0, raw, rah);
        CHECK(one.left == refL && one.right == refR, "integer luma bit-identical to the original float box filter");
        for (const char* t : {"2", "3", "7"}) {
            const rig::EyePair many = run(t, StereoLayout::SbsFull, swap != 0);
            CHECK(one.eyeW == many.eyeW && one.h == many.h, "same analysis size");
            CHECK(one.left == many.left && one.right == many.right, "threaded luma bit-identical to one thread");
            bool colourSame = true;
            for (int e = 0; e < 2; ++e)
                for (int k = 0; k < 3; ++k) colourSame = colourSame && one.meanRgb[e][k] == many.meanRgb[e][k];
            CHECK(colourSame, "threaded mean colours bit-identical to one thread");
            bool histSame = std::memcmp(one.hist, many.hist, sizeof(one.hist)) == 0 &&
                            one.colourSamples[0] == many.colourSamples[0] && one.colourSamples[1] == many.colourSamples[1];
            for (int e = 0; e < 2; ++e)
                histSame = histSame && std::memcmp(one.grid[e].data(), many.grid[e].data(), one.grid[e].size() * sizeof(float)) == 0;
            CHECK(histSame, "threaded histograms and colour grid bit-identical to one thread (#270)");
        }
    }
#if defined(_WIN32)
    _putenv_s("MEDIAPLAYER_RIG_THREADS", "");
#else
    unsetenv("MEDIAPLAYER_RIG_THREADS");
#endif
}

// #268: the dense map recovers each disc's disparity. The disc centres follow the test
// signal's motion (TestSignalSource.cpp DrawEye: base position + sway / bob, each eye
// shifted by half the disparity); the cells well inside a disc read its parallax.
void DenseMapRecoversDiscs() {
    struct D {
        float cx, cy, r;
    };
    const D base[TestSignalSource::kDiscs] = {{0.30f, 0.55f, 0.14f}, {0.55f, 0.40f, 0.12f}, {0.75f, 0.62f, 0.10f},
                                              {0.15f, 0.30f, 0.07f}};
    for (float scale : {1.0f, 1.5f}) {
        TestSignalConfig cfg;
        cfg.depthScale = scale;
        cfg.vShiftPx = 4.0f;  // a real rig's small vertical error must not lose the map
        const int64_t frame = 30;
        livepix::Nv12 px;
        TestSignalSource::Render(cfg, frame, px);
        rig::EyePair e;
        CHECK(rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, e), "dense: downsample");
        const rig::DenseMap m = rig::MeasureDense(e);
        std::printf("  dense x%.1f: %dx%d cells, %d matched, %d filled, %.2f ms\n", scale, m.cols, m.rows, m.matched,
                    m.filled, m.ms);
        CHECK(m.Ok() && m.cols >= 36 && m.rows >= 18, "dense: about 40 x 20 cells");
        const double t = (double)frame / cfg.fps;
        const float w = (float)cfg.eyeW, h = (float)cfg.eyeH;
        for (int d = 0; d < TestSignalSource::kDiscs; ++d) {
            const float want = TestSignalSource::kDisparity[d] * scale * 100.f;
            const float sway = 0.04f * (float)std::sin(t * (0.6 + 0.25 * d) + d);
            const float bob = 0.05f * (float)std::sin(t * (0.9 + 0.3 * d) + 2 * d);
            const float cx = ((base[d].cx + sway) * w - 0.5f * want / 100.f * w) / w;  // left eye, 0..1
            const float cy = (base[d].cy + bob);
            const float r = base[d].r * h;
            std::vector<float> in;
            for (int j = 0; j < m.rows; ++j)
                for (int i = 0; i < m.cols; ++i) {
                    const float p = m.pct[(size_t)j * m.cols + i];
                    const float dx = (m.u0 + i * m.du - cx) * w, dy = (m.v0 + j * m.dv - cy) * h;
                    if (!std::isnan(p) && dx * dx + dy * dy < 0.36f * r * r) in.push_back(p);
                }
            float got = 1e9f;
            if (!in.empty()) {
                std::nth_element(in.begin(), in.begin() + in.size() / 2, in.end());
                got = in[in.size() / 2];
            }
            std::printf("    disc %d: want %+.2f%% got %+.3f%% (%zu cells)\n", d, want, got, in.size());
            char msg[96];
            std::snprintf(msg, sizeof(msg), "dense x%.1f: disc %d within 0.1%% of width", scale, d);
            CHECK(!in.empty() && std::fabs(got - want) < 0.1f, msg);
        }
        // the histogram of the sparse matches peaks at each disc's depth
        const auto tm = std::chrono::steady_clock::now();
        const rig::Result r = rig::Measure(e);
        std::printf("    sparse Measure %.2f ms\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tm).count());
        const std::vector<int> hist = rig::Histogram(r.parallaxPct, rig::kHistLoPct, rig::kHistHiPct, rig::kParallaxHistBins);
        int total = 0;
        for (int c : hist) total += c;
        CHECK(total == (int)r.parallaxPct.size(), "histogram counts every match");
        for (int d = 0; d < TestSignalSource::kDiscs; ++d) {
            const float want = TestSignalSource::kDisparity[d] * scale * 100.f;
            const int b = std::clamp((int)std::floor((want - rig::kHistLoPct) * rig::kParallaxHistBins /
                                                     (rig::kHistHiPct - rig::kHistLoPct)), 0, rig::kParallaxHistBins - 1);
            int n = 0;
            for (int k = std::max(0, b - 1); k <= std::min(rig::kParallaxHistBins - 1, b + 1); ++k) n += hist[k];
            char msg[96];
            std::snprintf(msg, sizeof(msg), "histogram x%.1f: a peak at disc %d (%+.1f%%)", scale, d, want);
            CHECK(n >= 2, msg);
        }
        // the cursor probe at a disc centre reads that disc
        for (int d = 0; d < TestSignalSource::kDiscs; ++d) {
            const float want = TestSignalSource::kDisparity[d] * scale * 100.f;
            const float sway = 0.04f * (float)std::sin(t * (0.6 + 0.25 * d) + d);
            const float bob = 0.05f * (float)std::sin(t * (0.9 + 0.3 * d) + 2 * d);
            float pct = 0, conf = 0;
            const rig::Probe pr = rig::ProbeAt(e, base[d].cx + sway - 0.5f * want / 100.f, base[d].cy + bob, pct, conf);
            std::printf("    probe disc %d: state %d %+.3f%% (ncc %.2f)\n", d, (int)pr, pct, conf);
            char msg[96];
            std::snprintf(msg, sizeof(msg), "probe x%.1f: disc %d read", scale, d);
            CHECK(pr == rig::Probe::Ok && std::fabs(pct - want) < 0.15f, msg);
        }
    }
    // an untextured point reads "flat"
    {
        rig::EyePair e;
        e.eyeW = 640;
        e.h = 360;
        e.left.assign((size_t)640 * 360, 80.f);
        e.right = e.left;
        float pct, conf;
        CHECK(rig::ProbeAt(e, 0.5f, 0.5f, pct, conf) == rig::Probe::Flat, "probe: a flat area reads flat");
    }
}

// #268: readings in full-resolution DISPLAYED pixels - a half-SBS eye is shown at twice
// its stored width, so it reports the same px as the full-SBS master.
void DisplayedPixels() {
    TestSignalConfig cfg;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    rig::EyePair full, half;
    CHECK(rig::Downsample(ToFrame(px), StereoLayout::SbsFull, 640, full), "px: full downsamples");
    CHECK(rig::Downsample(ToFrame(Squeeze(px)), StereoLayout::SbsHalf, 640, half), "px: half downsamples");
    const rig::Result a = rig::Measure(full), b = rig::Measure(half);
    std::printf("  px: full near %+.2f%% = %+.1f px (%.2f px/%%), half near %+.2f%% = %+.1f px (%.2f px/%%), median %+.2f%%\n",
                a.nearPct, a.Px(a.nearPct), a.pxPerPct, b.nearPct, b.Px(b.nearPct), b.pxPerPct, a.medianPct);
    CHECK(std::fabs(a.pxPerPct - 19.2f) < 1e-3f, "px: full SBS 1920 px eye = 19.2 px per %");
    CHECK(std::fabs(b.pxPerPct - 19.2f) < 1e-3f, "px: half SBS 960 px stored, shown 1920 = 19.2 px per %");
    CHECK(std::fabs(a.Px(a.nearPct) - (-2.0f * 19.2f)) < 0.35f * 19.2f, "px: the front disc is ~-38 px");
    CHECK(std::fabs(a.Px(a.nearPct) - b.Px(b.nearPct)) < 0.3f * 19.2f, "px: half SBS reports displayed px like full");
    CHECK(a.medianPct >= a.nearPct && a.medianPct <= a.farPct, "median within near..far");
}

// #268 on the monitor: the dense pass runs only while enabled (and maps the measured pair),
// and the cursor probe answers off the caller's thread.
void MonitorDenseAndProbe() {
    TestSignalConfig cfg;
    livepix::Nv12 px;
    TestSignalSource::Render(cfg, 30, px);
    const std::vector<uint8_t> rgba = ToRgba(px);
    rig::RigMonitor m;
    m.SubmitStill(rgba.data(), px.width, px.height, StereoLayout::SbsFull, false);
    for (int i = 0; i < 500 && m.History(1e9).empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(m.Last().ok, "monitor: the still is measured");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(m.Dense().seq == 0, "monitor: no dense pass while the heatmap is off");
    m.SetDenseEnabled(true);
    for (int i = 0; i < 300 && !m.Dense().Ok(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const rig::DenseMap d = m.Dense();
    std::printf("  monitor dense: seq %llu, %d matched, %.2f ms\n", (unsigned long long)d.seq, d.matched, d.ms);
    CHECK(d.Ok(), "monitor: the dense map arrives once enabled");
    m.SetDenseEnabled(false);
    CHECK(!m.Dense().Ok(), "monitor: off clears the map");
    // the front disc (frame 30 of the test signal, its left-eye centre)
    const double t = 30.0 / cfg.fps;
    const float u = 0.30f + 0.04f * (float)std::sin(t * 0.6) + 0.5f * 0.02f;
    const float v = 0.55f + 0.05f * (float)std::sin(t * 0.9);
    rig::Probe s = rig::Probe::None;
    float pct = 0, conf = 0;
    CHECK(!m.ProbeResult(s, pct, conf), "monitor: no probe result before a request");
    m.RequestProbe(u, v);
    bool got = false;
    for (int i = 0; i < 300 && !(got = m.ProbeResult(s, pct, conf)); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::printf("  monitor probe: state %d %+.3f%%\n", (int)s, pct);
    CHECK(got && s == rig::Probe::Ok && std::fabs(pct + 2.0f) < 0.15f, "monitor: the probe thread reads the front disc");
}

int main() {
    ThreadedDownsampleIsExact();
    DenseMapRecoversDiscs();
    DisplayedPixels();
    MonitorDenseAndProbe();
    CleanRig();
    VerticalFault();
    ColourFault();
    DepthScale();
    MonoRefused();
    DeltaE();
    EyeSwap();
    RollFault();
    ScaleFault();
    ScaleNeedsCoverage();
    SeededSearch();
    ScaleAlert();
    ColourGains();
    ColourOverFrames();
    ColourOverlapMask();
    ColourLetterbox();
    ColourFixRoundTrip();
    ColourDiffMap();
    ColourAlert();
    StillsAndTopBottom();
    MonitorStill();
    TooFewMatches();
    EdgeViolations();
    EdgeAlert();
    DivergenceFollowsConvergence();
    AlertClosesOnBlack();
    SyncConfirmVotes();
    EyeSyncMeasured();
    int confirmedRight = 0;
    EyeSyncLikeTheApp(0.0f, confirmedRight);
    EyeSyncLikeTheApp(0.5f, confirmedRight);  // a rolled eye: dy runs across x, far more than a frame of motion
    CHECK(confirmedRight >= 1, "the 2-frame delay is confirmed from the pixels on the app's path");
    MeasuresWithDroppedFrames();
    if (g_failures) {
        std::fprintf(stderr, "rig_check_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rig_check_test: all passed\n");
    return 0;
}
