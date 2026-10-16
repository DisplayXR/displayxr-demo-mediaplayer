// SPDX-License-Identifier: Apache-2.0
//
// OpenColorIO in Media Player Pro's grade (docs/PRO.md Next 6): bake transforms from
// OCIO's built-in config into the grade's 3D LUT and check them with a trilinear lookup
// (the same sampling the shader does). Skipped when the build has no OpenColorIO.

#include "pro/OcioBake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

const char* kConfig = "ocio://default";  // OCIO's built-in CG config

// Trilinear lookup of a baked LUT (red fastest), domain 0..1.
void Sample(const mp::color::CubeLut& l, const float in[3], float out[3]) {
    const int n = l.size;
    float t[3], f[3];
    int i0[3];
    for (int k = 0; k < 3; ++k) {
        t[k] = std::fmin(std::fmax(in[k], 0.0f), 1.0f) * (n - 1);
        i0[k] = std::min((int)t[k], n - 2);
        f[k] = t[k] - (float)i0[k];
    }
    for (int ch = 0; ch < 3; ++ch) {
        float acc = 0.0f;
        for (int c = 0; c < 8; ++c) {
            const int r = i0[0] + (c & 1), g = i0[1] + ((c >> 1) & 1), b = i0[2] + ((c >> 2) & 1);
            const float w = ((c & 1) ? f[0] : 1 - f[0]) * ((c & 2) ? f[1] : 1 - f[1]) * ((c & 4) ? f[2] : 1 - f[2]);
            acc += w * l.rgb[(((size_t)b * n + g) * n + r) * 3 + ch];
        }
        out[ch] = acc;
    }
}

}  // namespace

int main() {
    if (!mp::ocio::Available()) {
        std::printf("ocio_bake_test: built without OpenColorIO - skipped\n");
        return 0;
    }
    mp::ocio::ConfigInfo info;
    std::string err;
    CHECK(mp::ocio::Describe(kConfig, info, err), err.c_str());
    std::printf("  config '%s': %zu colour spaces, %zu displays, default %s / %s\n", info.name.c_str(),
                info.colorSpaces.size(), info.displays.size(), info.defaultDisplay.c_str(), info.defaultView.c_str());
    CHECK(info.colorSpaces.size() > 10 && !info.defaultDisplay.empty(), "built-in config described");

    // linear -> sRGB-encoded: 18% grey -> 0.461
    mp::ocio::Request r;
    r.config = kConfig;
    r.src = "Linear Rec.709 (sRGB)";
    r.dst = "sRGB Encoded Rec.709 (sRGB)";
    r.size = 33;
    mp::color::CubeLut lut;
    CHECK(mp::ocio::Bake(r, lut, err), err.c_str());
    CHECK(lut.Valid() && lut.size == 33, "33^3 grid baked");
    const float grey[3] = {0.18f, 0.18f, 0.18f};
    float out[3] = {0, 0, 0};
    if (lut.Valid()) Sample(lut, grey, out);
    std::printf("  linear 0.18 -> sRGB %.3f (expect 0.461)\n", out[1]);
    CHECK(std::fabs(out[1] - 0.461f) < 0.01f, "linear 18% grey encodes to 0.461");
    const float white[3] = {1, 1, 1};
    if (lut.Valid()) Sample(lut, white, out);
    CHECK(std::fabs(out[0] - 1.0f) < 0.005f, "white stays white");

    // a display / view bake runs (ACEScct log -> the default display's default view)
    r.dst.clear();
    r.src = "ACEScct";
    CHECK(mp::ocio::Bake(r, lut, err), err.c_str());
    std::printf("  display/view bake: %s\n", lut.title.c_str());
    r.src = "No Such Space";
    CHECK(!mp::ocio::Bake(r, lut, err) && !err.empty(), "an unknown colour space is an error, not a crash");

    if (g_failures) {
        std::fprintf(stderr, "ocio_bake_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ocio_bake_test: all passed\n");
    return 0;
}
