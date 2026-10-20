// SPDX-License-Identifier: Apache-2.0
//
// pro::scopes: the CPU grade matches the shader's order and maths, the scopes read the
// left eye only, waveform / histogram / vectorscope bins land where the pixels say, and a
// grade change moves them.

#include "pro/Scopes.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace mp;
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

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

color::CubeLut Lut(int n, bool invert) {
    color::CubeLut l;
    l.size = n;
    l.rgb.resize((size_t)n * n * n * 3);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float* o = &l.rgb[(((size_t)b * n + g) * n + r) * 3];
                const float v[3] = {r / (float)(n - 1), g / (float)(n - 1), b / (float)(n - 1)};
                for (int k = 0; k < 3; ++k) o[k] = invert ? 1.0f - v[k] : v[k];
            }
    return l;
}

void GradeMaths() {
    scopes::Grade g;
    float c[3] = {0.25f, 0.5f, 0.75f};
    scopes::ApplyGrade(g, c);
    CHECK(Near(c[0], 0.25f) && Near(c[1], 0.5f) && Near(c[2], 0.75f), "identity grade leaves the colour");

    g.exposure = 1.0f;  // +1 stop in linear light, re-encoded
    float e[3] = {0.5f, 0.5f, 0.5f};
    scopes::ApplyGrade(g, e);
    CHECK(Near(e[0], LinearToSrgb(SrgbToLinear(0.5f) * 2.0f)), "exposure is applied in linear light");

    scopes::Grade cdl;
    cdl.cdlOn = true;
    cdl.slope[0] = cdl.slope[1] = cdl.slope[2] = 2.0f;
    float d[3] = {0.25f, 0.25f, 0.25f};
    scopes::ApplyGrade(cdl, d);
    CHECK(Near(d[0], 0.5f), "CDL slope");
    cdl.slope[0] = cdl.slope[1] = cdl.slope[2] = 1.0f;
    cdl.saturation = 0.0f;
    float s[3] = {1.0f, 0.0f, 0.0f};
    scopes::ApplyGrade(cdl, s);
    CHECK(Near(s[0], 0.2126f) && Near(s[1], 0.2126f), "CDL saturation 0 -> Rec.709 luma");

    const color::CubeLut id = Lut(17, false), inv = Lut(17, true);
    scopes::Grade lg;
    lg.lut = &id;
    float l[3] = {0.3f, 0.6f, 0.9f};
    scopes::ApplyGrade(lg, l);
    CHECK(Near(l[0], 0.3f) && Near(l[2], 0.9f), "identity LUT (trilinear) leaves the colour");
    lg.lut = &inv;
    float iv[3] = {0.3f, 0.6f, 0.9f};
    scopes::ApplyGrade(lg, iv);
    CHECK(Near(iv[0], 0.7f) && Near(iv[1], 0.4f) && Near(iv[2], 0.1f), "inverting LUT");

    // Order: exposure -> CDL -> LUT (exposure first, then the inverting LUT).
    scopes::Grade order;
    order.exposure = 1.0f;
    order.lut = &inv;
    float o[3] = {0.5f, 0.5f, 0.5f};
    scopes::ApplyGrade(order, o);
    CHECK(Near(o[0], 1.0f - LinearToSrgb(SrgbToLinear(0.5f) * 2.0f), 2e-3f), "exposure before the LUT");
}

// An RGBA still: left eye = left half dark grey / right half light grey; right eye red.
std::vector<uint8_t> Still(int w, int h) {
    std::vector<uint8_t> p((size_t)w * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* q = &p[((size_t)y * w + x) * 4];
            if (x >= w / 2) { q[0] = 255; q[1] = 0; q[2] = 0; }                 // right eye: red
            else if (x < w / 4) { q[0] = q[1] = q[2] = 64; }                   // left eye, left half
            else { q[0] = q[1] = q[2] = 191; }                                 // left eye, right half
        }
    return p;
}

void ScopesReadTheLeftEye() {
    const int W = 1280, H = 360;
    const std::vector<uint8_t> still = Still(W, H);
    std::vector<uint8_t> small;
    int w = 0, h = 0;
    CHECK(scopes::DownsampleRgba(still.data(), W, H, StereoLayout::SbsFull, 160, small, w, h), "downsample");
    CHECK(w == 160 && h == 90, "analysis size follows the eye's aspect (640x360 -> 160x90)");
    scopes::Result r;
    scopes::Measure(small, w, h, scopes::Grade{}, r);
    CHECK(r.ok, "measured");
    const int lo = (int)std::lround(64 / 255.0f * (scopes::kLevels - 1));
    const int hi = (int)std::lround(191 / 255.0f * (scopes::kLevels - 1));
    CHECK(r.waveY[(size_t)10 * scopes::kLevels + lo] == (uint32_t)h, "a left column: every pixel at the dark level");
    CHECK(r.waveY[(size_t)150 * scopes::kLevels + hi] == (uint32_t)h, "a right column: every pixel at the light level");
    CHECK(r.histR[255] == 0, "no red: the right eye is not measured");
    CHECK(r.histY[lo] + r.histY[hi] == (uint32_t)(w * h), "histogram: two peaks hold every pixel");
    const int mid = scopes::kVecSize / 2;
    uint32_t centre = 0;
    for (int y = mid - 1; y <= mid; ++y)
        for (int x = mid - 1; x <= mid; ++x) centre += r.vector[(size_t)y * scopes::kVecSize + x];
    CHECK(centre == (uint32_t)(w * h), "greys sit at the vectorscope centre");

    // A grade moves the scopes: +2 stops brightens the mean.
    scopes::Grade g;
    g.exposure = 2.0f;
    scopes::Result r2;
    scopes::Measure(small, w, h, g, r2);
    CHECK(r2.meanY > r.meanY + 0.15f, "exposure raises the measured luma");
    CHECK(r2.graded[0] > r.graded[0], "the probe's graded values follow the grade");
}

void VectorscopeHue() {
    // A pure-red left eye lands at the red target: Cb < 0, Cr > 0 (left of centre, above).
    const int W = 64, H = 32;
    std::vector<uint8_t> p((size_t)W * H * 4, 255);
    for (size_t i = 0; i < p.size(); i += 4) { p[i] = 255; p[i + 1] = 0; p[i + 2] = 0; }
    std::vector<uint8_t> small;
    int w = 0, h = 0;
    scopes::DownsampleRgba(p.data(), W, H, StereoLayout::Mono, 32, small, w, h);
    scopes::Result r;
    scopes::Measure(small, w, h, scopes::Grade{}, r);
    int bx = -1, by = -1;
    for (int y = 0; y < scopes::kVecSize; ++y)
        for (int x = 0; x < scopes::kVecSize; ++x)
            if (r.vector[(size_t)y * scopes::kVecSize + x]) { bx = x; by = y; }
    const float Y = 0.2126f, cb = (0 - Y) / 1.8556f, cr = (1 - Y) / 1.5748f;
    CHECK(bx == (int)((cb + 0.5f) * scopes::kVecSize) && by == (int)((0.5f - cr) * scopes::kVecSize),
          "red lands on the BT.709 red position");

    std::vector<uint8_t> img;
    scopes::RenderVectorscope(r, img);
    CHECK(img[((size_t)by * scopes::kVecSize + bx) * 4 + 1] > 0, "the vectorscope picture lights that bin");
    scopes::RenderWaveform(r, true, 96, img);
    CHECK(img.size() == (size_t)96 * scopes::kLevels * 4, "parade picture size");
    CHECK(img[((size_t)0 * 96 + 5) * 4 + 0] > 0, "parade: red at 100 % in the red third");
    CHECK(img[((size_t)0 * 96 + 37) * 4 + 1] == 0, "parade: no green at 100 %");
}

void FramesNeedPixels() {
    FrameRing::Frame f;
    f.width = 64;
    f.height = 32;
    f.gpu = true;  // zero-copy: no CPU planes
    std::vector<uint8_t> small;
    int w = 0, h = 0;
    CHECK(!scopes::Downsample(f, StereoLayout::Mono, 32, small, w, h), "a GPU-only frame has nothing to measure");
}

// The render-thread gather: left eye only, the eye's display aspect, colours intact (NV12),
// and the worker turns it into the same scopes the direct path gives.
void GatherAndWorker() {
    const int W = 1920, H = 540;  // half-SBS: each eye 960 wide, shown 1920 wide
    FrameRing::Frame f;
    f.width = W;
    f.height = H;
    f.format = PixFormat::NV12;
    f.plane[0].assign((size_t)W * H, 0);
    f.plane[1].assign((size_t)(W / 2) * (H / 2) * 2, 128);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) f.plane[0][(size_t)y * W + x] = x < W / 2 ? 63 : 32;  // left red, right blue
    for (int y = 0; y < H / 2; ++y)
        for (int x = 0; x < W / 2; ++x) {
            uint8_t* uv = &f.plane[1][((size_t)y * (W / 2) + x) * 2];
            if (x < W / 4) { uv[0] = 102; uv[1] = 240; } else { uv[0] = 240; uv[1] = 118; }
        }
    FrameRing::Frame mini;
    int w = 0, h = 0;
    CHECK(scopes::Gather(f, StereoLayout::SbsHalf, 160, mini, w, h), "gather");
    CHECK(w == 160 && h == 44, "half-SBS eye shown at 1920x540 -> 160x45, even -> 44");
    CHECK(mini.width == w * scopes::kGatherScale && mini.height == h * scopes::kGatherScale &&
              mini.format == PixFormat::NV12,
          "mini frame: kGatherScale x the analysis size, same format");
    std::vector<uint8_t> rgba;
    CHECK(scopes::DownsampleMini(mini, w, h, rgba), "downsample the mini frame");
    bool allRed = true;
    for (size_t i = 0; i < rgba.size(); i += 4) allRed = allRed && rgba[i] > 200 && rgba[i + 2] < 60;
    CHECK(allRed, "the gathered eye is the left (red) one, colours intact");

    scopes::ScopeWorker worker;
    scopes::ScopeWorker::Job job;
    job.mini = mini;
    job.w = w;
    job.h = h;
    job.hasFrame = true;
    worker.Submit(std::move(job));
    scopes::Result r;
    std::vector<uint8_t> wave, vec;
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        got = worker.Take(r, wave, vec);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got && r.ok && r.w == w && r.h == h, "the worker measures the gathered frame");
    CHECK(wave.size() == (size_t)480 * scopes::kLevels * 4 && vec.size() == (size_t)scopes::kVecSize * scopes::kVecSize * 4,
          "the worker renders the pictures");
    CHECK(r.histR[scopes::kLevels - 1] > (uint32_t)(w * h / 2), "red measured at full level");

    // A grade-only job re-measures the worker's last image without a new frame.
    scopes::ScopeWorker::Job g;
    g.grade.exposure = -2.0f;
    worker.Submit(std::move(g));
    scopes::Result r2;
    got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        got = worker.Take(r2, wave, vec);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got && r2.ok && r2.meanY < r.meanY - 0.05f, "a grade-only job re-measures the held image");
}

}  // namespace

int main() {
    GradeMaths();
    ScopesReadTheLeftEye();
    VectorscopeHue();
    FramesNeedPixels();
    GatherAndWorker();
    if (g_failures) {
        std::fprintf(stderr, "scopes_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("scopes_test: all passed\n");
    return 0;
}
