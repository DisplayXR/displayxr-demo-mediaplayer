// SPDX-License-Identifier: Apache-2.0
//
// VR180 / VR360 viewer (#155): the stereo pair stays RECTIFIED across the whole view range.
//
// Two complementary proofs on synthetic scenes (two cameras on a 63 mm horizontal
// baseline, points from 0.8 m to 50 m, +-85 degrees across and +-60 up / down):
//
//   1. Analytic: each scene point is projected into each eye's SOURCE picture with the
//      projection's forward map, then located in the rendered window by inverting the
//      window - and that location is checked to round-trip through vr::SampleUV (the exact
//      mapping shaders/sbs.frag runs), so the measurement is of the real code path. The
//      vertical disparity (row difference between the eyes, in pixels of a 1920x1080 eye)
//      must be ~0 at every pan / pitch / zoom in range, for VR180 equirect, dual fisheye
//      and ODS VR360. A naively ROTATED camera pair is measured too and must fail - the
//      negative control that shows the test can see the defect it guards against.
//
//   2. Pixels: a synthetic equirect SBS source with Gaussian dots is rendered through
//      vr::RenderEyeGray (the CPU twin of the shader); blob centroids are matched between
//      the eyes and their row difference measured, at the centre and at the pan clamp -
//      the rig-check measurement, on rendered windows.
//
// Plus the depth sanity (horizontal disparity positive in front, shrinking with distance,
// 0 at infinity), the clamps, the push-block packing, and the detection helpers.

#include "media/VrView.h"
#if defined(MEDIAPLAYER_WITH_FFMPEG)
#include "media/VideoStereoProbe.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mp;
using namespace mp::vr;

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

constexpr float kBaseline = 0.063f;
constexpr float kEyeW = 1920.f, kEyeH = 1080.f;
constexpr float kAspect = kEyeW / kEyeH;

struct P3 {
    float x, y, z;
};

// The synthetic scene: a lattice of directions x distances in front of the rig.
std::vector<P3> Scene() {
    std::vector<P3> pts;
    for (float az = -85.f; az <= 85.f; az += 5.f)
        for (float el = -60.f; el <= 60.f; el += 7.5f)
            for (float dist : {0.8f, 1.5f, 3.f, 8.f, 50.f}) {
                const float a = Rad(az), e = Rad(el);
                pts.push_back({dist * std::cos(e) * std::sin(a), dist * std::sin(e), dist * std::cos(e) * std::cos(a)});
            }
    return pts;
}

// --- forward source projection (independent of the viewer) -----------------------------

// A fixed-baseline camera at (cx, 0, 0): eye-local source uv of point p.
bool SourceUVFixed(Projection proj, float fishFov, const P3& p, float cx, float& u, float& v) {
    const float d[3] = {p.x - cx, p.y, p.z};
    return DirToLocalUV(proj, fishFov, 1.0f, d, u, v);
}

// ODS (omni-directional stereo): for eye sign sg (-1 left, +1 right) the ray of column
// azimuth th starts on the circle of radius r = b/2, perpendicular to th. Returns the
// eye-local uv of a full-sphere equirect and the elevation (same for both eyes by design).
bool SourceUVOds(const P3& p, float sg, float& u, float& v) {
    const float r = 0.5f * kBaseline;
    const float rho = std::sqrt(p.x * p.x + p.z * p.z);
    if (rho <= r) return false;
    const float phi = std::atan2(p.x, p.z);
    const float th = phi - sg * std::asin(r / rho);  // left eye (sg -1) looks further right
    const float el = std::atan2(p.y, std::sqrt(rho * rho - r * r));
    const float x = th / (2.f * kPi) + 0.5f;
    u = x - std::floor(x);
    v = 0.5f - el / kPi;
    return true;
}

// --- the window inverse (the measurement) ------------------------------------------------

// Where world direction d lands in the eye viewport, in (s, t) in [0,1]. Inverse of
// vr::RayDir + the window affine. False when behind the window.
bool WindowOf(const EyeWindow& w, const float d[3], float& s, float& t) {
    float tx, ty;
    if (w.model == Model::Cylinder) {
        const float a = std::atan2(d[0], d[2]) - w.headingRad;
        const float e = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
        float aw = std::fmod(a + 3.f * kPi, 2.f * kPi) - kPi;
        if (std::fabs(aw) >= 0.5f * kPi) return false;
        tx = std::tan(aw);
        ty = std::tan(e);
    } else {
        const float ch = std::cos(w.headingRad), sh = std::sin(w.headingRad);
        const float rx = d[0] * ch - d[2] * sh, ry = d[1], rz = d[0] * sh + d[2] * ch;  // Ry(-h)
        const float cp = std::cos(w.pitchRad), sp = std::sin(w.pitchRad);
        const float cy = ry * cp - rz * sp, cz = ry * sp + rz * cp;  // Rx(-p)
        if (cz <= 1e-6f) return false;
        tx = rx / cz;
        ty = cy / cz;
    }
    s = (tx - w.x0) / w.sx;
    t = (ty - w.y0) / w.sy;
    return true;
}

// Direction a source uv encodes (for the round trip): equirect / fisheye inverse.
void LocalUVToDir(Projection proj, float fishFov, float u, float v, float d[3]) {
    if (proj == Projection::Fisheye) {
        const float dx = u - 0.5f, dy = 0.5f - v;
        const float r = std::sqrt(dx * dx + dy * dy);
        const float th = r / 0.5f * 0.5f * Rad(fishFov);
        const float cs = r > 1e-9f ? dx / r : 0.f, sn = r > 1e-9f ? dy / r : 0.f;
        d[0] = std::sin(th) * cs;
        d[1] = std::sin(th) * sn;
        d[2] = std::cos(th);
        return;
    }
    const float lon = proj == Projection::Equirect180 ? (u - 0.5f) * kPi : (u - 0.5f) * 2.f * kPi;
    const float lat = (0.5f - v) * kPi;
    d[0] = std::cos(lat) * std::sin(lon);
    d[1] = std::sin(lat);
    d[2] = std::cos(lat) * std::cos(lon);
}

struct Disparity {
    float maxAbsDy = 0.f;   // vertical, px
    int measured = 0;       // points visible in both eyes
    float maxRoundTrip = 0.f;  // |SampleUV(window of p) - source uv of p|, in source uv
};

// One view: every scene point visible in both eyes -> row difference in px.
Disparity Measure(Projection proj, bool ods, float fishFov, const ViewState& view, float hfov, const Model* forceModel = nullptr) {
    Disparity out;
    EyeWindow wl = ComputeWindow(proj, true, view, hfov, kAspect, 0.f);
    EyeWindow wr = wl;
    if (forceModel) {  // the negative control: rotate a camera pair for yaw
        wl.model = wr.model = *forceModel;
        wl.x0 = wr.x0 = -std::tan(0.5f * Rad(hfov));
        wl.headingRad = wr.headingRad = Rad(view.yawDeg);
    }
    for (const P3& p : Scene()) {
        float st[2][2];
        bool ok = true;
        for (int e = 0; e < 2 && ok; ++e) {
            const EyeWindow& w = e == 0 ? wl : wr;
            float u, v;
            if (ods) ok = SourceUVOds(p, e == 0 ? -1.f : 1.f, u, v);
            else ok = SourceUVFixed(proj, fishFov, p, e == 0 ? -0.5f * kBaseline : 0.5f * kBaseline, u, v);
            if (!ok) break;
            float d[3];
            LocalUVToDir(proj, fishFov, u, v, d);  // the direction the SOURCE pixel encodes
            float s, t;
            if (!WindowOf(w, d, s, t) || s < 0.f || s > 1.f || t < 0.f || t > 1.f) { ok = false; break; }
            // Round trip through the real mapping: the window pixel samples that source uv.
            float ru, rv;
            if (!SampleUV(w, proj, fishFov, 1.0f, s, t, ru, rv)) { ok = false; break; }
            float du = std::fabs(ru - u);
            if (proj == Projection::Equirect360) du = std::min(du, 1.f - du);
            out.maxRoundTrip = std::max(out.maxRoundTrip, std::max(du, std::fabs(rv - v)));
            st[e][0] = s;
            st[e][1] = t;
        }
        if (!ok) continue;
        ++out.measured;
        out.maxAbsDy = std::max(out.maxAbsDy, std::fabs(st[0][1] - st[1][1]) * kEyeH);
    }
    return out;
}

void RectifiedAcrossRange(Projection proj, bool ods, const char* name) {
    const Limits lim;
    float worst = 0.f, worstRt = 0.f;
    int views = 0, points = 0;
    for (float hfov : {25.f, 40.f, 70.f}) {
        for (float yaw = -lim.panMaxDeg; yaw <= lim.panMaxDeg + 0.01f; yaw += 6.5f)
            for (float pitch = -lim.pitchMaxDeg; pitch <= lim.pitchMaxDeg + 0.01f; pitch += 10.f) {
                ViewState v{yaw, pitch, hfov};
                Clamp(v, proj, true, hfov, kAspect, lim);
                const Disparity d = Measure(proj, ods, 190.f, v, hfov);
                worst = std::max(worst, d.maxAbsDy);
                worstRt = std::max(worstRt, d.maxRoundTrip);
                points += d.measured;
                ++views;
            }
    }
    std::printf("  %-22s %d views, %d point pairs: max |vertical disparity| %.5f px, round trip %.2e\n", name,
                views, points, worst, worstRt);
    CHECK(points > 5000, "%s: too few points measured (%d)", name, points);
    CHECK(worst < 0.01f, "%s: vertical disparity %.4f px", name, worst);
    CHECK(worstRt < 2e-4f, "%s: window does not round-trip through SampleUV (%.2e)", name, worstRt);
}

void NaiveRotationFails() {
    // Rotating BOTH cameras for yaw (the thing the design refuses to do) at 45 degrees.
    const Model free = Model::Free;
    const Disparity d = Measure(Projection::Equirect180, false, 190.f, ViewState{45.f, 0.f, 40.f}, 40.f, &free);
    std::printf("  naive rotated pair @45deg: max |vertical disparity| %.2f px (must be large)\n", d.maxAbsDy);
    CHECK(d.measured > 100 && d.maxAbsDy > 2.0f, "the negative control should show vertical disparity (%.3f px)",
          d.maxAbsDy);
}

// Horizontal disparity (left x - right x, px) of a point straight down the window's axis.
float AxisDisparityPx(float yawDeg, float dist) {
    ViewState v{yawDeg, 0.f, 40.f};
    const EyeWindow w = ComputeWindow(Projection::Equirect180, true, v, 40.f, kAspect, 0.f);
    const float a = Rad(yawDeg);
    const P3 p{dist * std::sin(a), 0.f, dist * std::cos(a)};
    float s[2];
    for (int e = 0; e < 2; ++e) {
        float u, vv, d[3], t;
        SourceUVFixed(Projection::Equirect180, 0.f, p, e == 0 ? -0.5f * kBaseline : 0.5f * kBaseline, u, vv);
        LocalUVToDir(Projection::Equirect180, 0.f, u, vv, d);
        WindowOf(w, d, s[e], t);
    }
    return (s[0] - s[1]) * kEyeW;
}

void DepthSane() {
    for (float yaw : {0.f, 65.f}) {
        const float d1 = AxisDisparityPx(yaw, 1.f), d3 = AxisDisparityPx(yaw, 3.f), dinf = AxisDisparityPx(yaw, 1e5f);
        std::printf("  depth @yaw %2.0f: disparity 1 m %+.1f px, 3 m %+.1f px, infinity %+.3f px\n", yaw, d1, d3, dinf);
        CHECK(d1 > d3 && d3 > 0.f, "nearer must have more crossed disparity (yaw %.0f)", yaw);
        CHECK(std::fabs(dinf) < 0.05f, "infinity sits at zero disparity (yaw %.0f: %.3f)", yaw, dinf);
        // A plane window: disparity = f * b / Z with Z the depth along the plane normal
        // (dist * cos yaw) and f in px = W / (2 tan(hfov/2)): the pair keeps its full
        // baseline (no cos(yaw) collapse), the depth reads as a window onto the scene.
        const float f = kEyeW / (2.f * std::tan(Rad(20.f)));
        const float expect = f * kBaseline / (1.f * std::cos(Rad(yaw)));
        CHECK(std::fabs(d1 - expect) < 0.01f * expect, "1 m disparity %.2f vs window model %.2f (yaw %.0f)", d1,
              expect, yaw);
    }
}

// --- pixels -------------------------------------------------------------------------------

struct Blob {
    float x, y, mass;
    bool edge;  // touches the window border: cut, its centroid is not the dot's
};

std::vector<Blob> FindBlobs(const std::vector<uint8_t>& img, int w, int h) {
    std::vector<int> label((size_t)w * h, 0);
    std::vector<Blob> out;
    std::vector<int> stack;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (img[(size_t)y * w + x] < 40 || label[(size_t)y * w + x]) continue;
            double sx = 0, sy = 0, sm = 0;
            bool edge = false;
            stack.assign(1, y * w + x);
            label[(size_t)y * w + x] = 1;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int px = i % w, py = i / w;
                if (px == 0 || py == 0 || px == w - 1 || py == h - 1) edge = true;
                const double m = img[(size_t)i];
                sx += m * px;
                sy += m * py;
                sm += m;
                const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& n : nb) {
                    const int qx = px + n[0], qy = py + n[1];
                    if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                    const size_t q = (size_t)qy * w + qx;
                    if (label[q] || img[q] < 40) continue;
                    label[q] = 1;
                    stack.push_back(qy * w + qx);
                }
            }
            if (sm > 2000) out.push_back({(float)(sx / sm), (float)(sy / sm), (float)sm, edge});
        }
    return out;
}

void PixelRowsAlign() {
    // A VR180 equirect SBS source, 1024x1024 per eye, with Gaussian dots 0.6 deg wide on a
    // sparse direction grid at mixed distances.
    const int EW = 1024, EH = 1024, FW = 2 * EW, FH = EH;
    std::vector<uint8_t> src((size_t)FW * FH, 0);
    std::vector<P3> dots;
    int k = 0;
    for (float az = -80.f; az <= 80.f; az += 5.f)
        for (float el = -40.f; el <= 40.f; el += 5.f, ++k) {
            const float dist = (k % 3 == 0) ? 1.2f : (k % 3 == 1) ? 3.f : 20.f;
            const float a = Rad(az), e = Rad(el);
            dots.push_back({dist * std::cos(e) * std::sin(a), dist * std::sin(e), dist * std::cos(e) * std::cos(a)});
        }
    const float sigma = Rad(0.6f);
    for (int eye = 0; eye < 2; ++eye) {
        const float cx = eye == 0 ? -0.5f * kBaseline : 0.5f * kBaseline;
        std::vector<P3> dirs;
        for (const P3& p : dots) {
            const float x = p.x - cx, n = std::sqrt(x * x + p.y * p.y + p.z * p.z);
            dirs.push_back({x / n, p.y / n, p.z / n});
        }
        for (int y = 0; y < EH; ++y)
            for (int x = 0; x < EW; ++x) {
                float d[3];
                LocalUVToDir(Projection::Equirect180, 0.f, (x + 0.5f) / EW, (y + 0.5f) / EH, d);
                float best = 0.f;
                for (const P3& q : dirs) {
                    const float c = d[0] * q.x + d[1] * q.y + d[2] * q.z;
                    if (c < 0.999f) continue;
                    const float ang = std::acos(std::min(1.f, c));
                    best = std::max(best, std::exp(-0.5f * (ang * ang) / (sigma * sigma)));
                }
                src[(size_t)y * FW + eye * EW + x] = (uint8_t)(255.f * best + 0.5f);
            }
    }
    const int W = 960, H = 540;
    for (float yaw : {0.f, 32.5f, 65.f})
        for (float pitch : {0.f, 25.f}) {
            const ViewState v{yaw, pitch, 60.f};
            std::vector<uint8_t> img[2];
            for (int eye = 0; eye < 2; ++eye) {
                img[eye].assign((size_t)W * H, 0);
                const EyeWindow w = ComputeWindow(Projection::Equirect180, true, v, 60.f, (float)W / H, 0.f);
                RenderEyeGray(src.data(), FW, FH, FW, EyeRectFor(StereoLayout::SbsFull, eye == 1),
                              Projection::Equirect180, 0.f, w, img[eye].data(), W, H, W);
            }
            const std::vector<Blob> blobs[2] = {FindBlobs(img[0], W, H), FindBlobs(img[1], W, H)};
            const EyeWindow w = ComputeWindow(Projection::Equirect180, true, v, 60.f, (float)W / H, 0.f);
            // Match each dot to the blob nearest its predicted spot in each eye (the window
            // inverse), so a dense dot field cannot cross-match between the eyes.
            float worst = 0.f, minDx = 1e9f, maxDx = -1e9f, worstPred = 0.f;
            int matched = 0;
            for (const P3& p : dots) {
                const Blob* hit[2] = {nullptr, nullptr};
                for (int eye = 0; eye < 2; ++eye) {
                    float u, vv, d[3], ss, tt;
                    if (!SourceUVFixed(Projection::Equirect180, 0.f, p, eye == 0 ? -0.5f * kBaseline : 0.5f * kBaseline, u, vv))
                        break;
                    LocalUVToDir(Projection::Equirect180, 0.f, u, vv, d);
                    if (!WindowOf(w, d, ss, tt)) break;
                    const float px = ss * W - 0.5f, py = tt * H - 0.5f;
                    // Toward the clamp a dot is stretched up to ~1/cos^2 across, unevenly, so
                    // its centroid drifts sideways off the projected centre: match loosely in x,
                    // tightly in y (the axis under test).
                    float bd = 1e9f;
                    for (const Blob& c : blobs[eye]) {
                        if (c.edge || std::fabs(c.y - py) > 2.0f || std::fabs(c.x - px) > 16.0f) continue;
                        const float dd = std::fabs(c.x - px) + 4.0f * std::fabs(c.y - py);
                        if (dd < bd) { bd = dd; hit[eye] = &c; }
                    }
                    if (hit[eye]) worstPred = std::max(worstPred, std::fabs(hit[eye]->y - py));
                }
                if (!hit[0] || !hit[1]) continue;
                ++matched;
                worst = std::max(worst, std::fabs(hit[0]->y - hit[1]->y));
                minDx = std::min(minDx, hit[0]->x - hit[1]->x);
                maxDx = std::max(maxDx, hit[0]->x - hit[1]->x);
            }
            std::printf("  pixels yaw %4.1f pitch %2.0f: %2d dot pairs, max |dy| %.3f px, dx %+.1f..%+.1f px, "
                        "max |row - predicted row| %.2f px\n",
                        yaw, pitch, matched, worst, minDx, maxDx, worstPred);
            CHECK(matched >= 5, "too few dots matched at yaw %.1f pitch %.0f (%d)", yaw, pitch, matched);
            CHECK(worst < 0.35f, "rendered rows misaligned at yaw %.1f pitch %.0f: %.3f px", yaw, pitch, worst);
            CHECK(minDx > -0.5f, "a dot behind infinity (dx %.2f) at yaw %.1f", minDx, yaw);
            CHECK(worstPred < 1.0f, "rendered dots off their predicted row by %.2f px", worstPred);
        }
}

// --- the rest -----------------------------------------------------------------------------

void Clamps() {
    const Limits lim;
    ViewState v{100.f, 80.f, 0.f};
    Clamp(v, Projection::Equirect180, true, 40.f, kAspect, lim);
    CHECK(std::fabs(v.yawDeg - lim.panMaxDeg) < 1e-3f, "stereo 180 pan clamps to %.1f (got %.2f)", lim.panMaxDeg, v.yawDeg);
    CHECK(std::fabs(v.pitchDeg - lim.pitchMaxDeg) < 1e-3f, "pitch clamps (got %.2f)", v.pitchDeg);
    v = ViewState{190.f, 0.f, 0.f};
    Clamp(v, Projection::Equirect360, true, 40.f, kAspect, lim);
    CHECK(std::fabs(v.yawDeg + 170.f) < 1e-3f, "360 heading wraps (got %.2f)", v.yawDeg);
    v = ViewState{90.f, 0.f, 0.f};
    Clamp(v, Projection::Equirect180, false, 40.f, kAspect, lim);
    CHECK(std::fabs(v.yawDeg - 70.f) < 1e-2f, "mono 180 keeps the window inside the half sphere (got %.2f)", v.yawDeg);
    v = ViewState{0.f, 0.f, 500.f};
    Clamp(v, Projection::Equirect180, true, 40.f, kAspect, lim);
    CHECK(v.fovDeg == lim.fovMaxDeg, "zoom clamps");
    CHECK(std::fabs(LifeSizeHFovDeg(0.35f, 0.6f) - 32.52f) < 0.05f, "life-size FOV of a 35 cm window at 60 cm");
    CHECK(LifeSizeHFovDeg(0.f, 0.6f) == 40.f, "unknown size -> 40 deg");
}

void Packing() {
    const uint32_t f = PackModeFlags(Projection::Fisheye, Model::Plane, StereoLayout::SbsHalf, true, 190.f);
    CHECK(((f >> 8) & 7) == 3 && ((f >> 11) & 3) == 0 && ((f >> 13) & 3) == 1 && ((f >> 15) & 1) == 1 &&
              ((f >> 16) & 511) == 190 && (f >> 25) == 0 && (f & 0xFF) == 0,
          "mode flags round trip (0x%08x)", f);
    const uint32_t c = PackModeFlags(Projection::Eac, Model::Cylinder, StereoLayout::TbFull, false, 180.f, 100);
    CHECK(((c >> 8) & 7) == 5 && ((c >> 11) & 3) == 1 && ((c >> 13) & 3) == 2 && ((c >> 15) & 1) == 0 &&
              (c >> 25) == 100,
          "cube mode flags: 3-bit projection, padding in bits 25-31 (0x%08x)", c);
    CHECK((PackModeFlags(Projection::Cubemap, Model::Free, StereoLayout::Mono, false, 180.f, 999) >> 25) == 127,
          "padding saturates at 127");
    CHECK(PackModeFlags(Projection::None, Model::Plane, StereoLayout::SbsFull, true, 180.f) == 0,
          "flat = no flags (the base player is unchanged)");
    const uint32_t h = PackHeading(Rad(-90.f));
    CHECK((h & 0xFFFFu) == 0 && std::fabs(UnpackHeading(h) - Rad(270.f)) < 1e-3f, "heading unorm16");
    // The eye rects a VR source samples.
    const packing::EyeRect r = EyeRectFor(StereoLayout::TbHalf, true);
    CHECK(r.x == 0.f && r.y == 0.5f && r.w == 1.f && r.h == 0.5f, "TB second eye = bottom half");
}

void Detection() {
    Projection p;
    int pk;
    CHECK(ProjectionFromFilename("/x/y/Trip_VR180_LR.mp4", p, pk) && p == Projection::Equirect180 && pk == 1, "VR180 LR");
    CHECK(ProjectionFromFilename("clip_360_TB.mov", p, pk) && p == Projection::Equirect360 && pk == 2, "360 TB");
    CHECK(ProjectionFromFilename("cam_fisheye.mp4", p, pk) && p == Projection::Fisheye && pk == -1, "fisheye");
    CHECK(ProjectionFromFilename("beach_180x180_3dh.mp4", p, pk) && p == Projection::Equirect180 && pk == 1, "_180x180_3dh");
    CHECK(!ProjectionFromFilename("/videos/360/holiday.mp4", p, pk), "a folder name is not a hint");
    CHECK(!ProjectionFromFilename("IMG_0360.jpg", p, pk), "a frame number is not a hint");
    CHECK(PackingFromAspect(Projection::Equirect180, 2.f) == StereoLayout::SbsFull, "180 2:1 = SBS");
    CHECK(PackingFromAspect(Projection::Equirect180, 1.f) == StereoLayout::Mono, "180 1:1 = mono");
    CHECK(PackingFromAspect(Projection::Equirect360, 1.f) == StereoLayout::TbFull, "360 1:1 = TB");
    CHECK(PackingFromAspect(Projection::Equirect360, 2.f) == StereoLayout::Mono, "360 2:1 = mono");
    CHECK(PackingFromAspect(Projection::Equirect360, 4.f) == StereoLayout::SbsFull, "360 4:1 = SBS");
    CHECK(AspectPlausible(Projection::Equirect180, 2.f) && !AspectPlausible(Projection::Equirect180, 16.f / 9.f * 2.f / 1.5f),
          "aspect plausibility");
    CHECK(FromSpherical(1, 180.f) == Projection::Equirect180, "a bounded equirect tile of 180 deg");
    CHECK(FromSpherical(0, 360.f) == Projection::Equirect360, "a full equirect");
    CHECK(FromSpherical(2, 0.f) == Projection::Equirect180, "half equirect");
    CHECK(FromSpherical(3, 0.f) == Projection::Fisheye, "fisheye");
    CHECK(FromSpherical(4, 0.f) == Projection::Cubemap, "cubemap");
    CHECK(FromSpherical(9, 0.f) == Projection::None, "rectilinear etc. unsupported");
    CHECK(ParseProjectionId("equirect360", p) && p == Projection::Equirect360 && !ParseProjectionId("cube", p), "ids");
    // Cubemap / EAC: ids, names, hints, aspects.
    for (Projection q : {Projection::None, Projection::Equirect180, Projection::Equirect360, Projection::Fisheye,
                         Projection::Cubemap, Projection::Eac}) {
        Projection back = Projection::Equirect180;
        CHECK(ParseProjectionId(ProjectionId(q), back) && back == q, "id round trip %s", ProjectionId(q));
    }
    CHECK(ParseProjectionId("c3x2", p) && p == Projection::Cubemap, "c3x2 alias");
    CHECK(std::string(ProjectionName(Projection::Cubemap)) == "360 cubemap" &&
              std::string(ProjectionName(Projection::Eac)) == "360 EAC",
          "HUD names");
    CHECK(ProjectionFromFilename("city_cubemap.mp4", p, pk) && p == Projection::Cubemap && pk == -1, "_cubemap");
    CHECK(ProjectionFromFilename("city_360_c3x2_TB.mp4", p, pk) && p == Projection::Cubemap && pk == 2, "_c3x2 TB");
    CHECK(ProjectionFromFilename("yt_360_eac.webm", p, pk) && p == Projection::Eac && pk == -1, "_eac beats _360");
    CHECK(ProjectionFromFilename("stage_eac_sbs.mp4", p, pk) && p == Projection::Eac && pk == 1, "_eac SBS");
    CHECK(!ProjectionFromFilename("seac.mp4", p, pk), "eac only as a whole token");
    for (Projection q : {Projection::Cubemap, Projection::Eac}) {
        CHECK(PackingFromAspect(q, 1.5f) == StereoLayout::Mono, "cube 3:2 = mono");
        CHECK(PackingFromAspect(q, 0.75f) == StereoLayout::TbFull, "cube 3:4 = TB");
        CHECK(PackingFromAspect(q, 3.0f) == StereoLayout::SbsFull, "cube 3:1 = SBS");
        CHECK(AspectPlausible(q, 1.5f) && AspectPlausible(q, 0.75f) && AspectPlausible(q, 3.0f) &&
                  AspectPlausible(q, 3840.f / 2560.f) && !AspectPlausible(q, 2.0f),
              "cube aspect plausibility");
    }
    CHECK(AspectPlausible(Projection::Eac, 1920.f / 1080.f) && !AspectPlausible(Projection::Cubemap, 1920.f / 1080.f),
          "YouTube's 16:9 EAC frames are plausible EAC, not cubemap");
    CHECK(PackingFromAspect(Projection::Eac, 1920.f / 1080.f) == StereoLayout::Mono, "16:9 EAC without st3d = mono");
}

// --- cubemap / EAC ----------------------------------------------------------------------
//
// The reference is FFmpeg's v360 filter, transcribed independently of VrView.h: its
// pixel -> direction maps (cube3x2_to_xyz with the default face order "rludfb" and no
// rotation; eac_to_xyz), in v360's y-DOWN frame, turned into this file's y-up frame.

// v360 cube_to_xyz for direction `dir` (0 right, 1 left, 2 up, 3 down, 4 front, 5 back),
// face coords (uf, vf) in [-1, 1]; returns y-up.
void V360CubeToDir(int dir, float uf, float vf, float d[3]) {
    float x = 0, y = 0, z = 0;  // v360: y down
    switch (dir) {
        case 0: x = 1.f; y = vf; z = -uf; break;
        case 1: x = -1.f; y = vf; z = uf; break;
        case 2: x = uf; y = -1.f; z = vf; break;
        case 3: x = uf; y = 1.f; z = -vf; break;
        case 4: x = uf; y = vf; z = 1.f; break;
        default: x = -uf; y = vf; z = -1.f; break;
    }
    const float n = std::sqrt(x * x + y * y + z * z);
    d[0] = x / n;
    d[1] = -y / n;
    d[2] = z / n;
}

// c3x2 pixel (i, j) of a W x H eye -> direction. padPx = padding on each face edge.
void V360C3x2PixelDir(int i, int j, int W, int H, float padPx, float d[3]) {
    const float ew = W / 3.f, eh = H / 2.f;
    const int uFace = (int)std::floor(i / ew), vFace = (int)std::floor(j / eh);
    const int uShift = (int)std::ceil(ew * uFace), vShift = (int)std::ceil(eh * vFace);
    const int ewi = (int)std::ceil(ew * (uFace + 1)) - uShift, ehi = (int)std::ceil(eh * (vFace + 1)) - vShift;
    const float sw = 1.f - 2.f * padPx / ew, sh = 1.f - 2.f * padPx / eh;
    const float uf = ((2.f * (i - uShift) + 1.f) / ewi - 1.f) / sw;
    const float vf = ((2.f * (j - vShift) + 1.f) / ehi - 1.f) / sh;
    V360CubeToDir(uFace + 3 * vFace, uf, vf, d);
}

// v360 eac_to_xyz, verbatim but for the y flip. False on the 2-pixel margin (extrapolated).
bool V360EacPixelDir(int i, int j, int W, int H, float d[3]) {
    const float uPad = 2.f / W, vPad = 2.f / H;
    float uf = (i + 0.5f) / W, vf = (j + 0.5f) / H;
    uf = 3.f * (uf - uPad) / (1.f - 2.f * uPad);
    int uFace;
    bool margin = false;
    if (uf < 0.f) { uFace = 0; uf -= 0.5f; margin = true; }
    else if (uf >= 3.f) { uFace = 2; uf -= 2.5f; margin = true; }
    else { uFace = (int)std::floor(uf); uf = std::fmod(uf, 1.f) - 0.5f; }
    const int vFace = (int)std::floor(vf * 2.f);
    vf = (vf - vPad - 0.5f * vFace) / (0.5f - 2.f * vPad) - 0.5f;
    if (uf < -0.5f || uf >= 0.5f || vf < -0.5f || vf >= 0.5f) margin = true;
    uf = (uf >= -0.5f && uf < 0.5f) ? std::tan(0.5f * kPi * uf) : 2.f * uf;
    vf = (vf >= -0.5f && vf < 0.5f) ? std::tan(0.5f * kPi * vf) : 2.f * vf;
    float x, y, z;
    switch (uFace + 3 * vFace) {
        case 0: x = -1.f; y = vf; z = uf; break;   // TOP_LEFT: left
        case 1: x = uf; y = vf; z = 1.f; break;    // TOP_MIDDLE: front
        case 2: x = 1.f; y = vf; z = -uf; break;   // TOP_RIGHT: right
        case 3: x = -vf; y = 1.f; z = -uf; break;  // BOTTOM_LEFT: down
        case 4: x = -vf; y = -uf; z = -1.f; break; // BOTTOM_MIDDLE: back
        default: x = -vf; y = -1.f; z = uf; break; // BOTTOM_RIGHT: up
    }
    const float n = std::sqrt(x * x + y * y + z * z);
    d[0] = x / n;
    d[1] = -y / n;
    d[2] = z / n;
    return !margin;
}

void CubeFaceCentres() {
    // Directions of the six faces, in VrView's y-up frame, and the cell each lands in.
    const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const char* names[6] = {"right", "left", "up", "down", "front", "back"};
    const int c3x2Cell[6] = {0, 1, 2, 3, 4, 5};  // right left up / down front back
    const int eacCell[6] = {2, 0, 5, 3, 1, 4};   // left front right / down back up
    for (int k = 0; k < 6; ++k) {
        for (Projection p : {Projection::Cubemap, Projection::Eac}) {
            const int cell = p == Projection::Eac ? eacCell[k] : c3x2Cell[k];
            float u, v;
            CHECK(DirToLocalUV(p, 0.f, 1.5f, dirs[k], u, v), "cube always has a picture");
            const float eu = ((cell % 3) + 0.5f) / 3.f, ev = ((cell / 3) + 0.5f) / 2.f;
            CHECK(std::fabs(u - eu) < 1e-6f && std::fabs(v - ev) < 1e-6f, "%s %s centre: (%.4f, %.4f) want (%.4f, %.4f)",
                  ProjectionId(p), names[k], u, v, eu, ev);
        }
    }
    // Orientation on the front face (no rotation in either layout): up is up, right is right.
    float u0, v0, u1, v1;
    const float upish[3] = {0.f, 0.3f, 1.f}, rightish[3] = {0.3f, 0.f, 1.f};
    DirToLocalUV(Projection::Cubemap, 0.f, 1.5f, upish, u0, v0);
    DirToLocalUV(Projection::Cubemap, 0.f, 1.5f, rightish, u1, v1);
    CHECK(v0 < 0.75f && std::fabs(u0 - 0.5f) < 1e-6f && u1 > 0.5f && std::fabs(v1 - 0.75f) < 1e-6f,
          "front face: up is up, right is right");
    // Corners and edges: a cube corner lands on its face's corner (within the half-texel
    // inset), an edge direction on the face boundary.
    const float corner[3] = {1.f, 1.f, 1.f};  // right / up / front meet: the up face wins the tie
    float u, v;
    DirToLocalUV(Projection::Cubemap, 0.f, 1.5f, corner, u, v, 1536.f, 1024.f);
    // up face (cell 2): a = x / y = 1 (right edge), b = z / y = 1 (bottom edge)
    CHECK(std::fabs(u - (1.f - 0.5f / 1536.f)) < 1e-5f && std::fabs(v - (0.5f - 0.5f / 1024.f)) < 1e-5f,
          "cubemap corner (1,1,1) -> up face's bottom-right texel (%.5f, %.5f)", u, v);
    const float edge[3] = {1.f, 0.f, 1.f};  // front / right edge: the right face wins the tie
    DirToLocalUV(Projection::Cubemap, 0.f, 1.5f, edge, u, v, 1536.f, 1024.f);
    CHECK(std::fabs(u - 0.5f / 1536.f) < 1e-5f && std::fabs(v - 0.25f) < 1e-5f,
          "cubemap front/right edge -> the right face's left column (%.5f, %.5f)", u, v);
    DirToLocalUV(Projection::Eac, 0.f, 1.5f, edge, u, v, 1536.f, 1024.f);
    // EAC: right is cell 2 (top right); its left edge is the cell boundary at 2/3 (+ the margin share).
    const float eacEdge = 2.f / 1536.f + 2.f * (1.f - 4.f / 1536.f) / 3.f;
    CHECK(std::fabs(u - (eacEdge + 0.5f / 1536.f)) < 1e-5f && std::fabs(v - 0.25f) < 1e-5f,
          "eac front/right edge -> the right face's left column (%.5f, %.5f)", u, v);

    // EAC vs cubemap off-centre: 30 degrees right of the front centre. The cubemap places it
    // by tan (a / 2 of the face from the centre), EAC by angle (30/90 of the face).
    const float d30[3] = {std::sin(Rad(30.f)), 0.f, std::cos(Rad(30.f))};
    float uc, ue;
    DirToLocalUV(Projection::Cubemap, 0.f, 1.5f, d30, uc, v);
    DirToLocalUV(Projection::Eac, 0.f, 1.5f, d30, ue, v);
    const float fc = (uc * 3.f - 1.f) - 0.5f, fe = (ue * 3.f - 1.f) - 0.5f;  // offset from the face centre, in faces
    CHECK(std::fabs(fc - 0.5f * std::tan(Rad(30.f))) < 1e-5f, "cubemap: tan spacing (%.5f)", fc);
    CHECK(std::fabs(fe - 30.f / 90.f) < 1e-5f, "eac: equal-angle spacing (%.5f)", fe);
    CHECK(fe > fc, "EAC pushes an off-centre direction further out than the cubemap");

    // Stereo cubemap / EAC is the same ODS cylinder as stereo VR360; mono is free.
    for (Projection p : {Projection::Cubemap, Projection::Eac}) {
        CHECK(ModelFor(p, true) == Model::Cylinder && ModelFor(p, false) == Model::Free, "%s model", ProjectionId(p));
        ViewState vs{190.f, 0.f, 0.f};
        Clamp(vs, p, true, 40.f, kAspect, Limits{});
        CHECK(std::fabs(vs.yawDeg + 170.f) < 1e-3f, "%s heading wraps", ProjectionId(p));
    }
}

// Every pixel of a v360-made eye maps back to itself: v360's pixel -> direction, then
// VrView's direction -> uv lands on that pixel's centre.
void CubeRoundTripsV360() {
    // EAC faces a whole number of pixels: (W - 4) / 3 and H / 2 - 4 (v360's 2-px margins).
    const int W = 1536, H = 1024, WE = 1540, HE = 1032;
    float worstC = 0.f, worstE = 0.f, worstP = 0.f;
    int nE = 0;
    for (int j = 0; j < H; j += 3)
        for (int i = 0; i < W; i += 3) {
            float d[3], u, v;
            V360C3x2PixelDir(i, j, W, H, 0.f, d);
            DirToLocalUV(Projection::Cubemap, 0.f, 1.5f, d, u, v, (float)W, (float)H);
            worstC = std::max(worstC, std::max(std::fabs(u * W - 0.5f - i), std::fabs(v * H - 0.5f - j)));
            V360C3x2PixelDir(i, j, W, H, 8.f, d);  // 8 px of padding on each face edge
            const int fi = i % (W / 3), fj = j % (H / 2);
            if (fi >= 8 && fi < W / 3 - 8 && fj >= 8 && fj < H / 2 - 8) {  // the face proper
                DirToLocalUV(Projection::Cubemap, 0.f, 1.5f, d, u, v, (float)W, (float)H, 8.f);
                worstP = std::max(worstP, std::max(std::fabs(u * W - 0.5f - i), std::fabs(v * H - 0.5f - j)));
            }
        }
    for (int j = 0; j < HE; j += 3)
        for (int i = 0; i < WE; i += 3) {
            float d[3], u, v;
            if (V360EacPixelDir(i, j, WE, HE, d)) {
                DirToLocalUV(Projection::Eac, 0.f, 1.5f, d, u, v, (float)WE, (float)HE);
                worstE = std::max(worstE, std::max(std::fabs(u * WE - 0.5f - i), std::fabs(v * HE - 0.5f - j)));
                ++nE;
            }
        }
    std::printf("  cubemap / EAC vs FFmpeg v360: c3x2 %.4f px, c3x2 padded %.4f px, eac %.4f px (%d px)\n", worstC,
                worstP, worstE, nE);
    CHECK(worstC < 0.02f, "c3x2 round trip %.4f px", worstC);
    CHECK(worstP < 0.02f, "c3x2 padded round trip %.4f px", worstP);
    CHECK(nE > 150000 && worstE < 0.02f, "eac round trip %.4f px over %d px", worstE, nE);
}

// Pixels end to end: a smooth luma pattern over the sphere, written into a cubemap / EAC
// frame through v360's maps, rendered through windows by RenderEyeGray (the shader's CPU
// twin) must equal the pattern evaluated on each window ray - at every heading including
// straight up / down, so no face seam or misplaced face can hide. Stereo TB checks each
// eye reads its own half.
float Pattern(const float d[3], int eye) {
    const float lon = std::atan2(d[0], d[2]), lat = std::asin(std::max(-1.f, std::min(1.f, d[1])));
    const float g = 128.f + 70.f * std::cos(lat) * std::sin(3.f * lon) + 45.f * std::sin(2.f * lat) + 8.f * d[0];
    return eye == 0 ? g : 255.f - g;
}

void CubeRendersPattern() {
    struct Case {
        Projection p;
        StereoLayout l;
        bool youtube;  // YouTube's stereo EAC: side by side, each eye's grid turned 90 deg CCW
        const char* name;
    } cases[] = {{Projection::Cubemap, StereoLayout::Mono, false, "cubemap mono"},
                 {Projection::Cubemap, StereoLayout::TbFull, false, "cubemap TB"},
                 {Projection::Eac, StereoLayout::Mono, false, "eac mono"},
                 {Projection::Eac, StereoLayout::TbFull, false, "eac TB"},
                 {Projection::Eac, StereoLayout::SbsFull, true, "eac SBS (YouTube, portrait eyes)"}};
    for (const Case& c : cases) {
        const Projection p = c.p;
        const StereoLayout l = c.l;
        {
            // per eye: the upright grid is GW x GH; a YouTube eye is that turned (GH x GW)
            const int GW = 1152, GH = 768;
            const int W = c.youtube ? GH : GW, H = c.youtube ? GW : GH;
            const int eyes = l == StereoLayout::Mono ? 1 : 2;
            const bool sbs = l == StereoLayout::SbsFull;
            const int FW = sbs ? W * eyes : W, FH = sbs ? H : H * eyes;
            std::vector<uint8_t> src((size_t)FW * FH);
            for (int e = 0; e < eyes; ++e)
                for (int j = 0; j < H; ++j)
                    for (int i = 0; i < W; ++i) {
                        float d[3];
                        // turned 90 deg CCW: eye pixel (i, j) is upright-grid pixel (GW-1-j, i)
                        const int gi = c.youtube ? GW - 1 - j : i, gj = c.youtube ? i : j;
                        if (p == Projection::Cubemap) V360C3x2PixelDir(gi, gj, GW, GH, 0.f, d);
                        else V360EacPixelDir(gi, gj, GW, GH, d);
                        const size_t at = sbs ? (size_t)j * FW + e * W + i : (size_t)(e * H + j) * FW + i;
                        src[at] = (uint8_t)std::lround(Pattern(d, e));
                    }
            const int DW = 200, DH = 150;
            std::vector<uint8_t> dst((size_t)DW * DH);
            float worst = 0.f, sum = 0.f;
            int n = 0;
            const float views[][2] = {{0, 0}, {45, 0}, {90, 20}, {135, -30}, {180, 0}, {-100, 40},
                                      {-45, -45}, {30, 90}, {0, -90}, {-120, 89}};
            for (const auto& yp : views) {
                for (int e = 0; e < eyes; ++e) {
                    EyeWindow w = ComputeWindow(p, false, ViewState{yp[0], 0.f, 90.f}, 90.f, (float)DW / DH, 0.f);
                    w.pitchRad = Rad(yp[1]);  // Free model, the pitch set past the UI clamp (the poles)
                    RenderEyeGray(src.data(), FW, FH, FW, EyeRectFor(l, e == 1), p, 0.f, w, dst.data(), DW, DH, DW);
                    for (int y = 0; y < DH; ++y)
                        for (int x = 0; x < DW; ++x) {
                            float d[3];
                            RayDir(w, w.x0 + (x + 0.5f) / DW * w.sx, w.y0 + (y + 0.5f) / DH * w.sy, d);
                            const float n2 = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                            d[0] /= n2; d[1] /= n2; d[2] /= n2;
                            const float err = std::fabs((float)dst[(size_t)y * DW + x] - Pattern(d, e));
                            worst = std::max(worst, err);
                            sum += err;
                            ++n;
                        }
                }
            }
            std::printf("  %-32s rendered vs the pattern on the rays: mean %.3f, max %.2f (of 255)\n", c.name,
                        sum / n, worst);
            CHECK(sum / n < 0.8f && worst < 6.f, "%s: rendered pattern off (mean %.3f max %.2f)", c.name, sum / n,
                  worst);
        }
    }
}

#if defined(MEDIAPLAYER_WITH_FFMPEG)
// The container route: the committed fixtures (tools/vr/make_vr_fixtures.py --fixtures)
// carry Spherical Video V2 sv3d + st3d boxes; FFmpeg must hand them to VideoStereoProbe.
void ContainerMetadata() {
    struct Case {
        const char* file;
        Projection proj;
        StereoLayout layout;
    } cases[] = {{"vr180_sbs_sv3d.mov", Projection::Equirect180, StereoLayout::SbsFull},
                 {"vr360_tb_sv3d.mov", Projection::Equirect360, StereoLayout::TbFull},
                 {"cube360_mono_sv3d.mov", Projection::Cubemap, StereoLayout::Mono}};
    for (const Case& c : cases) {
        const std::string path = std::string(MP_FIXTURE_DIR) + "/" + c.file;
        int kind = -1;
        float cov = 0.f;
        int pad = -1;
        const bool have = VideoStereoProbe::ReadSpherical(path, kind, cov, &pad);
        CHECK(have && FromSpherical(kind, cov) == c.proj, "%s: spherical kind %d cover %.1f", c.file, kind, cov);
        CHECK(pad == (c.proj == Projection::Cubemap ? 2 : 0), "%s: cubemap padding %d", c.file, pad);
        const VideoStereoProbe::Result r = VideoStereoProbe::Run(path, false);
        CHECK(r.ok && r.haveSpherical && FromSpherical(r.sphericalKind, r.sphericalCoverageDeg) == c.proj,
              "%s: the probe's spherical verdict", c.file);
        CHECK(r.sphericalPadding == pad, "%s: the probe's padding", c.file);
        // st3d mono reads as "explicitly 2D" (metaLayout Mono).
        CHECK(r.haveMeta && r.metaLayout == c.layout, "%s: st3d -> packing (%d)", c.file, (int)r.metaLayout);
        std::printf("  %s: %s, %s\n", c.file, ProjectionName(FromSpherical(kind, cov)), have ? "sv3d read" : "-");
    }
}
#endif

}  // namespace

int main() {
    std::printf("vr_view_test (#155)\n");
    RectifiedAcrossRange(Projection::Equirect180, false, "VR180 equirect");
    RectifiedAcrossRange(Projection::Fisheye, false, "VR180 dual fisheye 190");
    RectifiedAcrossRange(Projection::Equirect360, true, "VR360 ODS (cylinder)");
    NaiveRotationFails();
    DepthSane();
    PixelRowsAlign();
    Clamps();
    Packing();
    Detection();
    CubeFaceCentres();
    CubeRoundTripsV360();
    CubeRendersPattern();
#if defined(MEDIAPLAYER_WITH_FFMPEG)
    ContainerMetadata();
#endif
    if (g_failures) {
        std::fprintf(stderr, "vr_view_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vr_view_test: all passed\n");
    return 0;
}
