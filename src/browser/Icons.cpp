// SPDX-License-Identifier: Apache-2.0
#include "browser/Icons.h"

namespace mp::browser {

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col) {
    const float th = s * 0.08f;  // stroke
    // A point at (x, y) in units of the box, from its centre.
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };

    switch (icon) {
        case Icon::Menu:
            for (int i = -1; i <= 1; ++i) {
                dl->AddRectFilled(P(-0.30f, i * 0.20f - 0.04f), P(0.30f, i * 0.20f + 0.04f), col, th);
            }
            break;
        case Icon::More:
            for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(P(0.0f, i * 0.22f), s * 0.065f, col);
            break;
        case Icon::AllMedia:
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 2; ++x) {
                    const float x0 = -0.28f + x * 0.32f, y0 = -0.28f + y * 0.32f;
                    dl->AddRectFilled(P(x0, y0), P(x0 + 0.24f, y0 + 0.24f), col, s * 0.04f);
                }
            }
            break;
        case Icon::Camera:
            dl->AddRectFilled(P(-0.13f, -0.30f), P(0.13f, -0.18f), col, s * 0.03f);
            dl->AddRect(P(-0.34f, -0.20f), P(0.34f, 0.26f), col, s * 0.07f, 0, th);
            dl->AddCircle(P(0.0f, 0.03f), s * 0.12f, col, 0, th);
            break;
        case Icon::Download:
            dl->AddLine(P(0.0f, -0.32f), P(0.0f, 0.10f), col, th);
            dl->PathLineTo(P(-0.17f, -0.06f));
            dl->PathLineTo(P(0.0f, 0.12f));
            dl->PathLineTo(P(0.17f, -0.06f));
            dl->PathStroke(col, 0, th);
            dl->AddRectFilled(P(-0.30f, 0.26f), P(0.30f, 0.26f + 0.08f), col, th * 0.5f);
            break;
        case Icon::Unsupported:
            dl->AddCircle(P(0.0f, 0.0f), s * 0.30f, col, 0, th);
            dl->AddLine(P(-0.21f, -0.21f), P(0.21f, 0.21f), col, th);
            break;
        case Icon::Files: {
            const ImVec2 page[] = {P(-0.25f, -0.33f), P(0.08f, -0.33f), P(0.25f, -0.16f), P(0.25f, 0.33f),
                                   P(-0.25f, 0.33f)};
            dl->AddPolyline(page, 5, col, ImDrawFlags_Closed, th);
            const ImVec2 fold[] = {P(0.08f, -0.33f), P(0.08f, -0.16f), P(0.25f, -0.16f)};
            dl->AddPolyline(fold, 3, col, 0, th);
            break;
        }
        case Icon::Image:
            dl->AddRect(P(-0.34f, -0.26f), P(0.34f, 0.26f), col, s * 0.06f, 0, th);
            dl->AddTriangleFilled(P(-0.24f, 0.17f), P(-0.04f, -0.07f), P(0.12f, 0.17f), col);
            dl->AddTriangleFilled(P(0.04f, 0.17f), P(0.15f, 0.03f), P(0.25f, 0.17f), col);
            dl->AddCircleFilled(P(0.15f, -0.12f), s * 0.05f, col);
            break;
        case Icon::Video:
            dl->AddRect(P(-0.34f, -0.26f), P(0.34f, 0.26f), col, s * 0.06f, 0, th);
            dl->AddTriangleFilled(P(-0.08f, -0.12f), P(-0.08f, 0.12f), P(0.13f, 0.0f), col);
            break;
    }
}

} // namespace mp::browser
