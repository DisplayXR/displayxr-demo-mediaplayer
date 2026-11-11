// SPDX-License-Identifier: Apache-2.0
#include "DepthWarp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

namespace mp {

namespace {

// A span wider than this (output pixels) between two neighbouring source pixels is a
// depth edge opening up, not a slanted surface.
constexpr float kEdgeGap = 2.0f;
constexpr int kChunkRows = 16;

void WarpRow(const uint8_t* src, const uint8_t* depth, int w, float halfShift, uint8_t* dst,
             std::vector<float>& pos, std::vector<int>& z) {
    for (int x = 0; x < w; ++x)
        pos[x] = (float)x + 0.5f + halfShift * ((float)depth[x] / 255.0f - 0.5f);
    std::fill(z.begin(), z.end(), -1);
    for (int x = 0; x + 1 < w; ++x) {
        const float p0 = pos[x], p1 = pos[x + 1];
        if (p1 <= p0) continue;  // folded under a nearer span
        const int o0 = std::max(0, (int)std::ceil(p0 - 0.5f));
        const int o1 = std::min(w, (int)std::ceil(p1 - 0.5f));
        const uint8_t* c0 = src + (size_t)x * 4;
        const uint8_t* c1 = c0 + 4;
        const int d0 = depth[x], d1 = depth[x + 1];
        const bool edge = p1 - p0 > kEdgeGap;
        const int zs = edge ? std::min(d0, d1) : std::max(d0, d1);
        const uint8_t* back = d0 <= d1 ? c0 : c1;
        for (int o = o0; o < o1; ++o) {
            if (zs < z[o]) continue;
            z[o] = zs;
            uint8_t* out = dst + (size_t)o * 4;
            if (edge) {
                std::memcpy(out, back, 4);
                continue;
            }
            const float t = ((float)o + 0.5f - p0) / (p1 - p0);
            for (int c = 0; c < 4; ++c) out[c] = (uint8_t)((float)c0[c] + t * (float)(c1[c] - c0[c]) + 0.5f);
        }
    }
    // Pixels no span reached (the row's ends, where the picture moved inwards) repeat
    // their neighbour.
    int first = 0;
    while (first < w && z[first] < 0) ++first;
    if (first == w) {
        std::memcpy(dst, src, (size_t)w * 4);
        return;
    }
    for (int o = 0; o < first; ++o) std::memcpy(dst + (size_t)o * 4, dst + (size_t)first * 4, 4);
    for (int o = first + 1; o < w; ++o)
        if (z[o] < 0) std::memcpy(dst + (size_t)o * 4, dst + (size_t)(o - 1) * 4, 4);
}

} // namespace

DecodedImage DepthWarp::MakeSbs(const DecodedImage& image, const std::vector<uint8_t>& depth,
                                float parallax) {
    DecodedImage out;
    const int w = image.width, h = image.height;
    out.width = 2 * w;
    out.height = h;
    out.pixels.resize((size_t)out.width * h * 4);
    const float halfShift = 0.5f * parallax * (float)w;  // +left eye, -right eye
    // Rows go out in small chunks, so a slow core (big.LITTLE) holds no one up.
    std::atomic<int> next{0};
    auto work = [&] {
        std::vector<float> pos(w);
        std::vector<int> z(w);
        for (int y0 = next.fetch_add(kChunkRows); y0 < h; y0 = next.fetch_add(kChunkRows)) {
            for (int y = y0; y < std::min(h, y0 + kChunkRows); ++y) {
                const uint8_t* src = image.pixels.data() + (size_t)y * w * 4;
                const uint8_t* d = depth.data() + (size_t)y * w;
                uint8_t* row = out.pixels.data() + (size_t)y * out.width * 4;
                WarpRow(src, d, w, halfShift, row, pos, z);
                WarpRow(src, d, w, -halfShift, row + (size_t)w * 4, pos, z);
            }
        }
    };
    const int n = (int)std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> pool;
    for (int i = 1; i < n; ++i) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    return out;
}

} // namespace mp
