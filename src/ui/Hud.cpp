// SPDX-License-Identifier: Apache-2.0
#include "ui/Hud.h"

#include "rhi/Font.h"

#include <algorithm>
#include <cstring>

namespace mp::hud {

namespace {
constexpr int kPadX = 10;  // panel padding around the text, in pixels
constexpr int kPadY = 6;

inline void Blend(uint8_t* p, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Straight-alpha "src over dst".
    const int ia = 255 - a;
    p[0] = (uint8_t)((r * a + p[0] * ia) / 255);
    p[1] = (uint8_t)((g * a + p[1] * ia) / 255);
    p[2] = (uint8_t)((b * a + p[2] * ia) / 255);
    p[3] = (uint8_t)(a + p[3] * ia / 255);
}
} // namespace

Rasterized RenderText(std::vector<uint8_t>& out, int bufW, int bufH, const char* text) {
    out.assign((size_t)bufW * bufH * 4, 0);  // fully transparent

    // Measure the text in monospace cells.
    int lines = 1, cols = 0, cur = 0;
    for (const char* c = text; *c; ++c) {
        if (*c == '\n') { ++lines; cols = std::max(cols, cur); cur = 0; }
        else ++cur;
    }
    cols = std::max(cols, cur);

    Rasterized r;
    r.width = std::min(bufW, cols * g_fontCellW + 2 * kPadX);
    r.height = std::min(bufH, lines * g_fontCellH + 2 * kPadY);

    // Translucent dark panel with a faint 1px border.
    for (int y = 0; y < r.height; ++y) {
        for (int x = 0; x < r.width; ++x) {
            const bool edge = (x == 0 || y == 0 || x == r.width - 1 || y == r.height - 1);
            uint8_t* p = out.data() + ((size_t)y * bufW + x) * 4;
            if (edge) { p[0] = 90; p[1] = 92; p[2] = 100; p[3] = 200; }
            else      { p[0] = 16; p[1] = 17; p[2] = 22;  p[3] = 188; }
        }
    }

    // Blit antialiased glyphs.
    int penX = kPadX, penY = kPadY;
    for (const char* c = text; *c; ++c) {
        if (*c == '\n') { penX = kPadX; penY += g_fontCellH; continue; }
        const unsigned ch = (unsigned char)*c;
        if (ch >= 32 && ch <= 126 && *c != ' ') {
            const uint8_t* glyph = g_fontAtlas[ch - 32];
            for (int gy = 0; gy < g_fontCellH; ++gy) {
                const int py = penY + gy;
                if (py < 0 || py >= bufH) continue;
                for (int gx = 0; gx < g_fontCellW; ++gx) {
                    const uint8_t cov = glyph[gy * g_fontCellW + gx];
                    if (!cov) continue;
                    const int px = penX + gx;
                    if (px < 0 || px >= bufW) continue;
                    Blend(out.data() + ((size_t)py * bufW + px) * 4, 240, 242, 248, cov);
                }
            }
        }
        penX += g_fontCellW;
    }
    return r;
}

} // namespace mp::hud
