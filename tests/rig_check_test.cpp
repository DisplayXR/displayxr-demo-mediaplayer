// SPDX-License-Identifier: Apache-2.0
//
// Rig checks (Media Player Pro, phase 2) against the synthetic rig, whose answers are
// known: its discs sit at fixed parallaxes (TestSignalSource::kDisparity), and the
// faults are dialled in. A clean rig must read as clean; each fault must be found.

#include "pro/LivePixels.h"
#include "pro/RigCheck.h"
#include "pro/TestSignalSource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        }
    }
#if defined(_WIN32)
    _putenv_s("MEDIAPLAYER_RIG_THREADS", "");
#else
    unsetenv("MEDIAPLAYER_RIG_THREADS");
#endif
}

int main() {
    ThreadedDownsampleIsExact();
    CleanRig();
    VerticalFault();
    ColourFault();
    DepthScale();
    MonoRefused();
    DeltaE();
    EyeSwap();
    RollFault();
    TooFewMatches();
    EdgeViolations();
    EdgeAlert();
    AlertClosesOnBlack();
    SyncConfirmVotes();
    EyeSyncMeasured();
    int confirmedRight = 0;
    EyeSyncLikeTheApp(0.0f, confirmedRight);
    EyeSyncLikeTheApp(0.5f, confirmedRight);  // a rolled eye: dy runs across x, far more than a frame of motion
    CHECK(confirmedRight >= 1, "the 2-frame delay is confirmed from the pixels on the app's path");
    if (g_failures) {
        std::fprintf(stderr, "rig_check_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rig_check_test: all passed\n");
    return 0;
}
