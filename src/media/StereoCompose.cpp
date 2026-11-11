// SPDX-License-Identifier: Apache-2.0
#include "StereoCompose.h"

#include "Log.h"
#include "media/AutoConvergence.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mp {

DecodedImage ComposeSbs(const DecodedImage& left, const DecodedImage& right) {
    DecodedImage out;
    if (!left.Valid() || !right.Valid()) return out;
    if (left.width != right.width || left.height != right.height) return out;

    const int w = left.width, h = left.height;
    const size_t rowBytes = (size_t)w * 4;
    out.width = w * 2;
    out.height = h;
    out.pixels.resize((size_t)w * 2 * h * 4);
    uint8_t* dst = out.pixels.data();
    for (int y = 0; y < h; ++y) {
        const size_t rowStart = (size_t)y * (size_t)(w * 2) * 4;
        std::copy_n(left.pixels.data() + (size_t)y * rowBytes, rowBytes, dst + rowStart);
        std::copy_n(right.pixels.data() + (size_t)y * rowBytes, rowBytes,
                    dst + rowStart + rowBytes);
    }
    return out;
}

float EstimateAutoConvergence(const DecodedImage& sbs) {
    float shift = 0.0f;
    const char* rule = "none";
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = autoconv::MeasureStillShift(sbs.pixels.data(), sbs.width, sbs.height, shift, &rule);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ok) {
        LOG_INFO("auto-convergence: subject (%s) %+.4f in %.0f ms", rule, shift, ms);
        return shift;
    }
    const int w = sbs.width / 2, h = sbs.height;
    if (w < 16 || h < 16) return 0.0f;
    constexpr int DW = 96;                       // downsample width
    const int DH = std::max(8, DW * h / w);
    auto gray = [&sbs](int sx, int sy) {
        const uint8_t* p = &sbs.pixels[((size_t)sy * sbs.width + sx) * 4];
        return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    };
    std::vector<float> gl((size_t)DW * DH), gr((size_t)DW * DH);
    for (int y = 0; y < DH; ++y)
        for (int x = 0; x < DW; ++x) {
            const int sx = x * w / DW, sy = y * h / DH;
            gl[(size_t)y * DW + x] = gray(sx, sy);
            gr[(size_t)y * DW + x] = gray(w + sx, sy);
        }
    const int maxD = DW / 5;                      // search ±20% disparity
    float bestSad = 1e30f;
    int bestD = 0;
    for (int d = -maxD; d <= maxD; ++d) {
        float sad = 0.0f;
        int cnt = 0;
        const int x0 = std::max(0, -d), x1 = std::min(DW, DW - d);
        for (int y = 0; y < DH; ++y)
            for (int x = x0; x < x1; ++x) {
                sad += std::fabs(gl[(size_t)y * DW + x] - gr[(size_t)y * DW + x + d]);
                ++cnt;
            }
        if (cnt > 0) {
            sad /= (float)cnt;
            if (sad < bestSad) { bestSad = sad; bestD = d; }
        }
    }
    const float dFrac = (float)bestD / (float)DW;  // right−left disparity, fraction of width
    // Applying convergence_ reduces the dominant disparity by 2*conv, so conv = dFrac/2
    // pulls that plane to zero. (Yields a negative value for the usual pair, matching the
    // sign of the containers that do carry a convergence field.)
    LOG_INFO("auto-convergence: no confident subject, coarse SAD %+.4f", 0.5f * dFrac);
    return 0.5f * dFrac;
}

} // namespace mp
