// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for auto-convergence (#92). Mirrors displayxr-web test/call-autoconv.test.mjs:
// synthetic SBS pairs with KNOWN disparities, d = x_left - x_right, positive = in front of
// the display plane. Plus what this port adds: the pluggable subject policies, the comfort
// clamp, the rate limit / scene-cut snap, DEFAULT OFF, and the sign through the same shift
// expression the draw code uses (autoconv::ConvergenceShiftPx).

#include "media/AutoConvergence.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using namespace mp::autoconv;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL (%s:%d): ", __FILE__, __LINE__);  \
            std::fprintf(stderr, __VA_ARGS__);                             \
            std::fprintf(stderr, "\n");                                    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

using Tex = std::function<int(int, int)>;
using Inside = std::function<bool(int, int)>;

// Deterministic texture: a value per (x, y) in `cell`-px cells, seeded.
Tex Texture(uint32_t seed, int cell = 4) {
    return [seed, cell](int x, int y) {
        const int cx = (int)std::floor((double)x / cell), cy = (int)std::floor((double)y / cell);
        uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u ^ seed * 83492791u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return 40 + (int)(h % 176u);
    };
}

struct Layer {
    int d;
    Tex tex;
    Inside inside;  // LEFT-eye coordinates; empty = everywhere
};

// A SBS gray image; layers drawn back to front. The right eye sees each layer shifted
// left by d (the right-eye pixel x shows the left-eye point x + d).
std::vector<uint8_t> Sbs(int E, int H, const std::vector<Layer>& layers) {
    const int W = 2 * E;
    std::vector<uint8_t> img((size_t)W * H);
    for (int eye = 0; eye < 2; ++eye)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < E; ++x) {
                int v = 128;
                for (const Layer& L : layers) {
                    const int xl = eye == 0 ? x : x + L.d;
                    if (!L.inside || L.inside(xl, y)) v = L.tex(xl, y);
                }
                img[(size_t)y * W + eye * E + x] = (uint8_t)v;
            }
    return img;
}

Inside Rect(int x0, int y0, int x1, int y1) {
    return [=](int x, int y) { return x >= x0 && x < x1 && y >= y0 && y < y1; };
}
Inside Ellipse(double cx, double cy, double rx, double ry) {
    return [=](int x, int y) {
        const double a = (x - cx) / rx, b = (y - cy) / ry;
        return a * a + b * b <= 1.0;
    };
}

Measurement MeasureFD(const std::vector<uint8_t>& img, int E, int H, const float* focus = nullptr) {
    return MeasureFocusDisparity(SbsGray{img.data(), 2 * E, H}, SearchOptions{}, focus, nullptr);
}

// ---- mirrors of call-autoconv.test.mjs --------------------------------------------------

void TestSquareInFront() {
    const int E = 240, H = 135;
    auto img = Sbs(E, H, {{0, Texture(1), {}}, {48, Texture(2), Rect(90, 35, 150, 95)}});
    const Measurement m = MeasureFD(img, E, H);
    CHECK(m.ok, "square: measured");
    CHECK(std::fabs(m.d - 48) <= 1, "square: d=%.2f", m.d);
    CHECK(m.method == Method::Mode, "square: method mode");
}

void TestPersonHeadWins() {
    const int E = 240, H = 180;
    auto img = Sbs(E, H, {{6, Texture(3), {}},
                          {24, Texture(4), Rect(60, 110, 180, 180)},
                          {30, Texture(5), Ellipse(120, 70, 38, 50)}});
    const Measurement m = MeasureFD(img, E, H);
    CHECK(m.ok, "person: measured");
    CHECK(std::fabs(m.d - 30) <= 1.5, "person: d=%.2f", m.d);
    CHECK(m.x > 85 && m.x < 155, "person: x=%.1f", m.x);
    CHECK(m.y > 25 && m.y < 95, "person: y=%.1f", m.y);
}

void TestGivenFocus() {
    const int E = 240, H = 180;
    auto img = Sbs(E, H, {{6, Texture(3), {}}, {30, Texture(5), Ellipse(120, 70, 38, 50)}});
    const float f[2] = {120.f, 60.f};
    const Measurement m = MeasureFD(img, E, H, f);
    CHECK(m.ok && m.method == Method::Focus, "focus: measured via focus");
    CHECK(std::fabs(m.d - 30) <= 1, "focus: d=%.2f", m.d);
}

void TestVerticalMisalignment() {
    const int E = 240, H = 135, W = 2 * E;
    auto img = Sbs(E, H, {{0, Texture(1), {}}, {40, Texture(2), Rect(80, 30, 160, 100)}});
    for (int y = H - 1; y >= 2; --y)  // drop the right eye 2 rows
        std::memcpy(&img[(size_t)y * W + E], &img[(size_t)(y - 2) * W + E], (size_t)E);
    const Measurement m = MeasureFD(img, E, H);
    CHECK(m.ok && std::fabs(m.d - 40) <= 1.5, "misaligned: ok=%d d=%.2f", m.ok, m.d);
}

void TestFlatFrame() {
    const int E = 240, H = 135;
    std::vector<uint8_t> img((size_t)2 * E * H, 12);
    CHECK(!MeasureFD(img, E, H).ok, "flat: nothing measured");
}

void TestNearestModeShare() {
    std::vector<Block> blocks;
    for (int i = 0; i < 20; ++i) blocks.push_back(Block{0, 0, 5.f});
    for (int i = 0; i < 8; ++i) blocks.push_back(Block{0, 0, 30.f});
    blocks.push_back(Block{0, 0, 60.f});
    float d = 0;
    CHECK(NearestMode(blocks, 2, 0.12f, 3, d) && d == 30.f, "nearestMode: d=%.1f", d);
    CHECK(!NearestMode({}, 2, 0.12f, 3, d), "nearestMode: empty");
}

void TestBlocksPositiveForCrossed() {
    const int E = 160, H = 90;
    auto img = Sbs(E, H, {{12, Texture(7), {}}});
    auto b = BlockDisparities(SbsGray{img.data(), 2 * E, H});
    CHECK(b.size() > 10, "blocks: %zu", b.size());
    std::vector<float> ds;
    for (auto& x : b) ds.push_back(x.d);
    std::sort(ds.begin(), ds.end());
    const float med = ds.empty() ? 0.f : ds[ds.size() >> 1];
    CHECK(std::fabs(med - 12) <= 0.6, "blocks: median %.2f", med);
}

void TestDisparityTrack() {
    DisparityTrack t;
    CHECK(!t.Push(false, 0), "track: nothing yet");
    t.Push(true, 40);
    t.Push(true, 44);
    t.Push(true, 90);
    CHECK(t.Value() == 44.f, "track: outlier does not move it (%.1f)", t.Value());
    CHECK(t.Push(false, 0) && t.Value() == 44.f, "track: blink holds");
    t.Reset();
    CHECK(!t.HasValue(), "track: reset");
}

void TestTrackerFollowsHead() {
    const int E = 240, H = 180;
    auto frame = [&](int cx, int d) {
        return Sbs(E, H, {{6, Texture(3), {}},
                          {d, [cx](int x, int y) { return Texture(5)(x - (cx - 120), y); },
                           Ellipse(cx, 70, 38, 50)}});
    };
    FocusTracker tr;
    auto a = frame(120, 30);
    const Measurement m0 = tr.Measure(SbsGray{a.data(), 2 * E, H}, 0, {}, nullptr, nullptr);
    CHECK(m0.ok && m0.method == Method::Mode && std::fabs(m0.d - 30) <= 1.5,
          "tracker lock: ok=%d d=%.2f", m0.ok, m0.d);
    auto b = frame(128, 32);
    const Measurement m1 = tr.Measure(SbsGray{b.data(), 2 * E, H}, 200, {}, nullptr, nullptr);
    CHECK(m1.ok && m1.method == Method::Track, "tracker: tracked (method %s)", MethodName(m1.method));
    CHECK(std::fabs(m1.d - 32) <= 1, "tracker: d=%.2f", m1.d);
    std::vector<uint8_t> flat((size_t)2 * E * H, 10);
    CHECK(!tr.Measure(SbsGray{flat.data(), 2 * E, H}, 400, {}, nullptr, nullptr).ok,
          "tracker: lost -> nothing");
}

void TestTrackerReSearch() {
    const int E = 240, H = 180;
    auto f = Sbs(E, H, {{6, Texture(3), {}}, {30, Texture(5), Ellipse(120, 70, 38, 50)}});
    FocusTracker::Config c;
    c.fullEveryMs = 1000;
    FocusTracker tr(c);
    const SbsGray g{f.data(), 2 * E, H};
    CHECK(tr.Measure(g, 0, {}, nullptr, nullptr).method == Method::Mode, "re-search: t0 mode");
    CHECK(tr.Measure(g, 500, {}, nullptr, nullptr).method == Method::Track, "re-search: t500 track");
    CHECK(tr.Measure(g, 1500, {}, nullptr, nullptr).method == Method::Mode, "re-search: t1500 mode");
}

void TestPeriodicBackground() {
    const int E = 240, H = 135;
    Tex checker = [](int x, int y) {
        return ((int)std::floor(x / 15.0) + (int)std::floor(y / 15.0)) % 2 ? 70 : 150;
    };
    auto img = Sbs(E, H, {{0, checker, {}}, {20, Texture(9), Rect(90, 35, 150, 95)}});
    const Measurement m = MeasureFD(img, E, H);
    CHECK(m.ok && std::fabs(m.d - 20) <= 1, "periodic bg: ok=%d d=%.2f (aliased?)", m.ok, m.d);
}

void TestPeriodicOnlyRejected() {
    // Nothing but a 30-px-period checker: every block matches equally well one period away,
    // so the uniqueness test must reject all of them.
    const int E = 240, H = 135;
    Tex checker = [](int x, int y) {
        return ((int)std::floor(x / 15.0) + (int)std::floor(y / 15.0)) % 2 ? 70 : 150;
    };
    auto img = Sbs(E, H, {{0, checker, {}}});
    auto b = BlockDisparities(SbsGray{img.data(), 2 * E, H});
    CHECK(b.empty(), "periodic only: %zu blocks survived uniqueness", b.size());
    CHECK(!MeasureFD(img, E, H).ok, "periodic only: no subject");
}

void TestDownsample() {
    std::vector<uint8_t> src(3 + 9 * 4, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 7; ++x) src[3 + y * 9 + x] = y < 2 ? 100 : x < 2 ? 40 : 200;
    std::vector<uint8_t> out;
    int w, h;
    DownsampleLuma(src.data() + 3, 9, 7, 4, 2, false, out, w, h);
    CHECK(w == 2 && h == 2, "ds box: %dx%d", w, h);
    CHECK(out.size() == 4 && out[0] == 100 && out[1] == 100 && out[2] == 40 && out[3] == 200,
          "ds box values");
    std::vector<uint8_t> s2(12 * 6);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 12; ++x) s2[(size_t)y * 12 + x] = x < 6 ? 60 : 180;
    DownsampleLuma(s2.data(), 12, 12, 6, 6, false, out, w, h);
    CHECK(w == 2 && h == 1 && out[0] == 60 && out[1] == 180, "ds 2x2 sample");
    DownsampleLuma(s2.data(), 12, 12, 6, 6, true, out, w, h);
    CHECK(out[0] == 180 && out[1] == 60, "ds swapEyes exchanges halves");
    CHECK(DownsampleFactorFor(3840) == 8 && DownsampleFactorFor(1920) == 4 &&
              DownsampleFactorFor(480) == 1,
          "ds factor");
}

// ---- what the port adds ------------------------------------------------------------------

// A full-resolution plane (x `scale`) with padding in the stride, through Measure().
std::vector<uint8_t> Upscale(const std::vector<uint8_t>& g, int W, int H, int scale, int stride) {
    std::vector<uint8_t> out((size_t)stride * H * scale, 0);
    for (int y = 0; y < H * scale; ++y)
        for (int x = 0; x < W * scale; ++x) out[(size_t)y * stride + x] = g[(size_t)(y / scale) * W + x / scale];
    return out;
}

void TestKnownDisparityFullRes() {
    const int E = 240, H = 135, S = 4;
    for (int d : {24, -8}) {
        auto g = Sbs(E, H, {{d, Texture(11), {}}});
        const int stride = 2 * E * S + 64;
        auto full = Upscale(g, 2 * E, H, S, stride);
        for (mp::StereoLayout lay : {mp::StereoLayout::SbsFull, mp::StereoLayout::SbsHalf}) {
            DisparityEstimator est;
            const Result r = est.Measure(full.data(), stride, 2 * E * S, H * S, lay,
                                         SubjectPolicy::Nearest, nullptr, 0.0);
            const float want = (float)d / (float)E;
            CHECK(r.ok && std::fabs(r.disparityFrac - want) < 0.006f,
                  "full-res d=%d layout=%d: ok=%d frac=%.4f want %.4f (%s)", d, (int)lay, r.ok,
                  r.disparityFrac, want, r.rule);
        }
    }
    {
        // Eye swap: the viewer's left eye now sees the right half -> the sign flips.
        auto g = Sbs(E, H, {{8, Texture(12), {}}});
        DisparityEstimator est;
        const Result a = est.Measure(g.data(), 2 * E, 2 * E, H, mp::StereoLayout::SbsFull,
                                     SubjectPolicy::Nearest, nullptr, 0.0, /*swap=*/false);
        DisparityEstimator est2;
        const Result b = est2.Measure(g.data(), 2 * E, 2 * E, H, mp::StereoLayout::SbsFull,
                                      SubjectPolicy::Nearest, nullptr, 0.0, /*swap=*/true);
        CHECK(a.ok && b.ok && std::fabs(a.disparityFrac + b.disparityFrac) < 0.005f &&
                  a.disparityFrac > 0,
              "swap flips the sign: %.4f vs %.4f", a.disparityFrac, b.disparityFrac);
    }
    {
        DisparityEstimator est;
        std::vector<uint8_t> g((size_t)2 * E * H, 90);
        const Result r = est.Measure(g.data(), 2 * E, 2 * E, H, mp::StereoLayout::Mono,
                                     SubjectPolicy::Nearest, nullptr, 0.0);
        CHECK(!r.ok && std::strcmp(r.rule, "mono") == 0, "mono is not measured");
    }
}

void TestNoTextureHolds() {
    const int E = 240, H = 135;
    auto g = Sbs(E, H, {{24, Texture(13), {}}});
    std::vector<uint8_t> flat((size_t)2 * E * H, 20);
    DisparityEstimator est;
    const Result a = est.MeasureGray(g.data(), 2 * E, H, SubjectPolicy::Nearest, nullptr, 0);
    const Result b = est.MeasureGray(flat.data(), 2 * E, H, SubjectPolicy::Nearest, nullptr, 200);
    CHECK(a.ok && a.hasValue, "hold: first measured");
    CHECK(!b.ok, "hold: flat frame measures nothing");
    CHECK(b.hasValue && b.disparityFrac == a.disparityFrac, "hold: value held (%.4f vs %.4f)",
          b.disparityFrac, a.disparityFrac);
    CHECK(b.sceneCut, "hold: covering the camera reads as a scene cut (mad %.1f)", b.sceneMad);
    DisparityEstimator fresh;
    const Result c = fresh.MeasureGray(flat.data(), 2 * E, H, SubjectPolicy::Nearest, nullptr, 0);
    CHECK(!c.ok && !c.hasValue, "no texture, nothing held -> no value");
}

void TestPoliciesPickDifferentPlanes() {
    // A: near (d=30), coarse texture, left.  B: mid (d=16), FINE texture, right.
    // C: background (d=4), coarse texture, owns the centre.
    const int E = 240, H = 180;
    auto img = Sbs(E, H, {{4, Texture(21, 6), {}},
                          {30, Texture(22, 6), Rect(30, 20, 80, 160)},
                          {16, Texture(23, 3), Rect(160, 20, 210, 160)}});
    auto run = [&](SubjectPolicy p) {
        DisparityEstimator est;
        return est.MeasureGray(img.data(), 2 * E, H, p, nullptr, 0);
    };
    const Result n = run(SubjectPolicy::Nearest);
    const Result c = run(SubjectPolicy::Centre);
    const Result s = run(SubjectPolicy::Sharpness);
    CHECK(n.ok && std::fabs(n.disparityFrac * E - 30) <= 1.5, "policy nearest: d=%.2f (%s)",
          n.disparityFrac * E, n.rule);
    CHECK(c.ok && std::fabs(c.disparityFrac * E - 4) <= 1.5, "policy centre: d=%.2f (%s)",
          c.disparityFrac * E, c.rule);
    CHECK(s.ok && std::fabs(s.disparityFrac * E - 16) <= 1.5 && std::strcmp(s.rule, "sharp") == 0,
          "policy sharp: d=%.2f (%s)", s.disparityFrac * E, s.rule);
    CHECK(n.hasRange && n.nearFrac * E > 28 && n.farFrac * E < 6,
          "range: near %.1f far %.1f", n.nearFrac * E, n.farFrac * E);
    // Uniform sharpness -> Sharpness falls through to Centre.
    auto uni = Sbs(E, H, {{4, Texture(24), {}}, {30, Texture(25), Rect(36, 20, 96, 160)}});
    DisparityEstimator est;
    const Result u = est.MeasureGray(uni.data(), 2 * E, H, SubjectPolicy::Sharpness, nullptr, 0);
    CHECK(u.ok && std::strcmp(u.rule, "sharp>centre") == 0 && std::fabs(u.disparityFrac * E - 4) <= 1.5,
          "uniform sharpness falls through: rule %s d=%.2f", u.rule, u.disparityFrac * E);
    // Focus: a pinned point on B picks B even under nearest's nearer A.
    DisparityEstimator ef;
    const float fuv[2] = {185.f / E, 90.f / H};
    const Result f = ef.MeasureGray(img.data(), 2 * E, H, SubjectPolicy::Focus, fuv, 0);
    CHECK(f.ok && std::fabs(f.disparityFrac * E - 16) <= 1.5 && std::strcmp(f.rule, "focus") == 0,
          "policy focus: d=%.2f (%s)", f.disparityFrac * E, f.rule);
}

Result Synthetic(float d, float nearF, float farF) {
    Result r;
    r.ok = r.hasValue = r.hasRange = true;
    r.disparityFrac = d;
    r.nearFrac = nearF;
    r.farFrac = farF;
    return r;
}

void TestComfortClamp() {
    CHECK(!AutoConvergenceController::Config{}.comfortClamp, "the comfort clamp is opt-in (default off)");
    AutoConvergenceController::Config on;
    on.comfortClamp = true;  // opt-in: the default is off
    AutoConvergenceController c(on);
    const char* why = "";
    // Scene spans 0.02 .. 0.12, subject at 0.10: rear pulls forward, front then wins.
    float s = c.TargetFor(Synthetic(0.10f, 0.12f, 0.02f), &why);
    CHECK(std::fabs(s - (-0.0575f)) < 1e-5f && std::strcmp(why, "front") == 0,
          "clamp front wins: s=%.4f (%s)", s, why);
    CHECK(0.12f + 2 * s <= 0.005f + 1e-5f, "clamp: nearest content within the front budget");
    // Narrow scene: no clamp, the subject goes to the plane.
    s = c.TargetFor(Synthetic(0.10f, 0.10f, 0.095f), &why);
    CHECK(std::fabs(s + 0.05f) < 1e-6f && *why == 0, "no clamp: s=%.4f (%s)", s, why);
    // Subject in front of nothing much, far content deep behind: rear pulls forward.
    // (rear binds when d - far > rear; front stays free while near - far <= front + rear)
    s = c.TargetFor(Synthetic(0.04f, 0.039f, 0.02f), &why);
    CHECK(std::strcmp(why, "rear") == 0 && std::fabs(0.02f + 2 * s + 0.015f) < 1e-5f,
          "rear clamp: s=%.4f (%s)", s, why);
    // The web's +-12 % shift clamp.
    AutoConvergenceController::Config cfg;
    cfg.comfortClamp = false;
    AutoConvergenceController c2(cfg);
    CHECK(std::fabs(c2.TargetFor(Synthetic(0.5f, 0.5f, 0.5f), &why) + 0.12f) < 1e-6f,
          "max shift clamp");
}

void TestTemporal() {
    AutoConvergenceController::Config cfg;
    cfg.comfortClamp = false;
    AutoConvergenceController c(cfg);
    const double dt = 1.0 / 60.0;
    // DEFAULT OFF: no update, ticking disabled -> exactly 0.
    for (int i = 0; i < 120; ++i) c.Tick(dt, false);
    CHECK(c.Value() == 0.f, "default off: exactly 0");
    // First lock skips the rate limit: reaches -0.05 in a few seconds.
    c.Update(Synthetic(0.10f, 0.10f, 0.10f));
    for (int i = 0; i < 60 * 5; ++i) c.Tick(dt, true);
    CHECK(std::fabs(c.Value() + 0.05f) < 0.001f, "first lock snaps: %.4f", c.Value());
    // A new target without a scene cut is rate limited to 0.2 %/s.
    c.Update(Synthetic(0.02f, 0.02f, 0.02f));  // target -0.01
    const float v0 = c.Value();
    for (int i = 0; i < 60; ++i) c.Tick(dt, true);
    CHECK(std::fabs(c.Value() - v0) <= 0.002f + 1e-5f && c.Value() > v0,
          "rate limit: moved %.5f in 1 s", c.Value() - v0);
    // A scene cut skips it once.
    Result cut = Synthetic(0.02f, 0.02f, 0.02f);
    cut.sceneCut = true;
    c.Update(cut);
    for (int i = 0; i < 60 * 4; ++i) c.Tick(dt, true);
    CHECK(std::fabs(c.Value() + 0.01f) < 0.001f, "scene cut snaps: %.4f", c.Value());
    // Dropouts (no value) keep the target.
    Result none;
    c.Update(none);
    CHECK(std::fabs(c.Target() + 0.01f) < 1e-6f, "dropout keeps the target");
    // Off: glides back, then lands on exactly 0.
    c.Tick(dt, false);
    CHECK(c.Value() != 0.f, "off ramps (not a snap)");
    for (int i = 0; i < 60 * 5; ++i) c.Tick(dt, false);
    CHECK(c.Value() == 0.f, "off lands on exactly 0 (%g)", c.Value());
}

void TestSignThroughUvMath() {
    // Measure a pair whose subject is 24 px in front (E = 240), converge, then place a
    // subject point through the draw code's shift: both eyes must land on the same pixel.
    const int E = 240, H = 135;
    const int dTrue = 24;
    auto g = Sbs(E, H, {{dTrue, Texture(31), {}}});
    DisparityEstimator est;
    const Result r = est.MeasureGray(g.data(), 2 * E, H, SubjectPolicy::Nearest, nullptr, 0);
    AutoConvergenceController c;
    c.Update(r);
    for (int i = 0; i < 600; ++i) c.Tick(1.0 / 60.0, true);
    const float s = c.Value();
    CHECK(s < 0.f, "in-front subject pushes back (negative shift): %.4f", s);
    const uint32_t contentW = 1920;  // on-screen eye width
    const float scale = (float)contentW / (float)E;
    const float xL = 120.f * scale, xR = (120.f - dTrue) * scale;  // same point, both eyes
    const float screenL = xL + (float)ConvergenceShiftPx(s, contentW, /*isLeftView=*/true);
    const float screenR = xR + (float)ConvergenceShiftPx(s, contentW, /*isLeftView=*/false);
    CHECK(std::fabs(screenL - screenR) <= 2.f, "zero parallax on screen: L %.1f R %.1f",
          screenL, screenR);
    // Before converging, the same point had +d on-screen disparity (crossed).
    CHECK(xL - xR > 0, "unshifted subject is crossed");
}

}  // namespace

int main() {
    TestSquareInFront();
    TestPersonHeadWins();
    TestGivenFocus();
    TestVerticalMisalignment();
    TestFlatFrame();
    TestNearestModeShare();
    TestBlocksPositiveForCrossed();
    TestDisparityTrack();
    TestTrackerFollowsHead();
    TestTrackerReSearch();
    TestPeriodicBackground();
    TestPeriodicOnlyRejected();
    TestDownsample();
    TestKnownDisparityFullRes();
    TestNoTextureHolds();
    TestPoliciesPickDifferentPlanes();
    TestComfortClamp();
    TestTemporal();
    TestSignThroughUvMath();
    if (g_failures) {
        std::fprintf(stderr, "auto_convergence_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "auto_convergence_test: all passed\n");
    return 0;
}
