// SPDX-License-Identifier: Apache-2.0
#include "ui/Icons.h"

#include "ui/TransportUI.h"

#include <cmath>

namespace mp::ui {

namespace {
constexpr float kPi = 3.14159265358979f;  // IM_PI lives in imgui_internal.h; keep our own
constexpr float kStroke = 0.11f;          // the desktop's stroke, of the box
}  // namespace

void DrawGlyph(ImDrawList* dl, Glyph glyph, ImVec2 c, float size, ImU32 u, bool filled) {
    const float r = size * 0.30f;
    const float th = size * kStroke;
    // A stroke: the one touch stroke, or on desktop `th` times the part's own weight.
    auto T = [&](float weight) { return g_iconStroke > 0.0f ? g_iconStroke : th * weight; };
    // A point at (x, y) in units of the box, from its centre.
    auto P = [&](float x, float y) { return ImVec2(c.x + x * size, c.y + y * size); };

    switch (glyph) {
        // ---- The transport: the desktop's geometry, unchanged ----
        case Glyph::Play: {
            const float h = size * 0.30f;
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.7f, c.y - h), ImVec2(c.x - h * 0.7f, c.y + h),
                                  ImVec2(c.x + h, c.y), u);
            break;
        }
        case Glyph::Pause: {
            const float bw = size * 0.13f, bh = size * 0.30f, gap = size * 0.10f;
            dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh), ImVec2(c.x - gap, c.y + bh), u, th * 0.4f);
            dl->AddRectFilled(ImVec2(c.x + gap, c.y - bh), ImVec2(c.x + gap + bw, c.y + bh), u, th * 0.4f);
            break;
        }
        case Glyph::Loop: {
            // Two arcs forming a near-circle, with a small arrowhead at each open end.
            dl->PathArcTo(c, r, kPi * 0.30f, kPi * 0.95f);
            dl->PathStroke(u, 0, T(1.0f));
            dl->PathArcTo(c, r, kPi * 1.30f, kPi * 1.95f);
            dl->PathStroke(u, 0, T(1.0f));
            const float ah = size * 0.13f;
            const ImVec2 e1(c.x + r * std::cos(kPi * 0.95f), c.y + r * std::sin(kPi * 0.95f));
            dl->AddTriangleFilled(ImVec2(e1.x - ah, e1.y), ImVec2(e1.x + ah * 0.4f, e1.y - ah),
                                  ImVec2(e1.x + ah * 0.4f, e1.y + ah), u);
            const ImVec2 e2(c.x + r * std::cos(kPi * 1.95f), c.y + r * std::sin(kPi * 1.95f));
            dl->AddTriangleFilled(ImVec2(e2.x + ah, e2.y), ImVec2(e2.x - ah * 0.4f, e2.y - ah),
                                  ImVec2(e2.x - ah * 0.4f, e2.y + ah), u);
            break;
        }
        case Glyph::Slideshow: {
            // A photo frame with a small play triangle inside (auto-advancing stills).
            dl->AddRect(ImVec2(c.x - r, c.y - r * 0.78f), ImVec2(c.x + r, c.y + r * 0.78f), u, size * 0.10f, 0,
                        T(0.8f));
            const float h = size * 0.16f;
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.5f, c.y - h), ImVec2(c.x - h * 0.5f, c.y + h),
                                  ImVec2(c.x + h, c.y), u);
            break;
        }
        case Glyph::Speaker:
        case Glyph::SpeakerMuted: {
            // Body box + cone (triangle pointing right).
            const float bx = -r * 0.35f;
            dl->AddRectFilled(ImVec2(c.x - r * 0.95f, c.y - r * 0.30f), ImVec2(c.x + bx, c.y + r * 0.30f), u);
            dl->AddTriangleFilled(ImVec2(c.x + bx, c.y - r * 0.62f), ImVec2(c.x + bx, c.y + r * 0.62f),
                                  ImVec2(c.x + r * 0.25f, c.y), u);
            if (glyph == Glyph::Speaker) {  // two sound-wave arcs
                dl->PathArcTo(ImVec2(c.x + r * 0.1f, c.y), r * 0.58f, -kPi * 0.28f, kPi * 0.28f);
                dl->PathStroke(u, 0, T(0.7f));
                dl->PathArcTo(ImVec2(c.x + r * 0.1f, c.y), r * 0.92f, -kPi * 0.28f, kPi * 0.28f);
                dl->PathStroke(u, 0, T(0.7f));
            } else {  // muted: a small X
                const float xc = c.x + r * 0.72f, d = r * 0.26f;
                dl->AddLine(ImVec2(xc - d, c.y - d), ImVec2(xc + d, c.y + d), u, T(0.9f));
                dl->AddLine(ImVec2(xc - d, c.y + d), ImVec2(xc + d, c.y - d), u, T(0.9f));
            }
            break;
        }
        case Glyph::SkipBack:
        case Glyph::SkipForward: {
            // Most of a circle, open at the top, its arrowhead on the side it turns to, and
            // "10" in the middle.
            const bool fwd = glyph == Glyph::SkipForward;
            const float a0 = fwd ? -kPi * 0.42f : -kPi * 0.58f;
            const float a1 = fwd ? kPi * 1.30f : -kPi * 2.30f;
            const float rr = size * 0.34f;
            dl->PathArcTo(c, rr, a0, a1, 32);
            dl->PathStroke(u, 0, T(1.0f));
            const ImVec2 e(c.x + rr * std::cos(a0), c.y + rr * std::sin(a0));
            const float ah = size * 0.10f, dir = fwd ? 1.0f : -1.0f;
            dl->AddTriangleFilled(ImVec2(e.x + dir * ah * 1.2f, e.y), ImVec2(e.x - dir * ah * 0.4f, e.y - ah),
                                  ImVec2(e.x - dir * ah * 0.4f, e.y + ah), u);
            ImFont* font = StyleFont(TextStyle::Small);
            const float px = font ? font->FontSize : ImGui::GetFontSize() * 0.7f;
            ImFont* f = font ? font : ImGui::GetFont();
            const ImVec2 ts = f->CalcTextSizeA(px, 1e9f, 0.0f, "10");
            dl->AddText(f, px, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), u, "10");
            break;
        }
        case Glyph::Info:
            dl->AddCircle(c, size * 0.32f, u, 0, T(1.0f));
            dl->AddCircleFilled(P(0.0f, -0.15f), size * 0.045f, u);
            dl->AddLine(P(0.0f, -0.05f), P(0.0f, 0.18f), u, T(1.0f));
            break;

        // ---- Navigation ----
        case Glyph::Back: {
            dl->AddLine(P(-0.26f, 0.0f), P(0.28f, 0.0f), u, T(1.0f));
            const ImVec2 head[] = {P(-0.04f, -0.22f), P(-0.26f, 0.0f), P(-0.04f, 0.22f)};
            dl->AddPolyline(head, 3, u, 0, T(1.0f));
            break;
        }
        case Glyph::Menu:
            for (int i = -1; i <= 1; ++i) dl->AddLine(P(-0.30f, i * 0.20f), P(0.30f, i * 0.20f), u, T(1.0f));
            break;
        case Glyph::More:
            for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(P(0.0f, i * 0.22f), size * 0.065f, u);
            break;
        case Glyph::Close:
            dl->AddLine(P(-0.24f, -0.24f), P(0.24f, 0.24f), u, T(1.0f));
            dl->AddLine(P(0.24f, -0.24f), P(-0.24f, 0.24f), u, T(1.0f));
            break;
        case Glyph::Delete: {
            dl->AddLine(P(-0.30f, -0.21f), P(0.30f, -0.21f), u, T(1.0f));  // the lid
            const ImVec2 handle[] = {P(-0.10f, -0.21f), P(-0.10f, -0.31f), P(0.10f, -0.31f), P(0.10f, -0.21f)};
            dl->AddPolyline(handle, 4, u, 0, T(1.0f));
            const ImVec2 body[] = {P(-0.22f, -0.21f), P(-0.19f, 0.32f), P(0.19f, 0.32f), P(0.22f, -0.21f)};
            dl->AddPolyline(body, 4, u, 0, T(1.0f));
            break;
        }

        // ---- Sources and tiles ----
        case Glyph::AllMedia:
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 2; ++x) {
                    const float x0 = -0.28f + x * 0.32f, y0 = -0.28f + y * 0.32f;
                    if (filled) {
                        dl->AddRectFilled(P(x0, y0), P(x0 + 0.24f, y0 + 0.24f), u, size * 0.04f);
                    } else {
                        dl->AddRect(P(x0, y0), P(x0 + 0.24f, y0 + 0.24f), u, size * 0.04f, 0, T(1.0f));
                    }
                }
            }
            break;
        case Glyph::Camera: {
            const ImVec2 bump[] = {P(-0.16f, -0.20f), P(-0.10f, -0.30f), P(0.10f, -0.30f), P(0.16f, -0.20f)};
            dl->AddPolyline(bump, 4, u, 0, T(1.0f));
            dl->AddRect(P(-0.34f, -0.20f), P(0.34f, 0.26f), u, size * 0.07f, 0, T(1.0f));
            if (filled) {
                dl->AddCircleFilled(P(0.0f, 0.03f), size * 0.12f, u);
            } else {
                dl->AddCircle(P(0.0f, 0.03f), size * 0.12f, u, 0, T(1.0f));
            }
            break;
        }
        case Glyph::Download: {
            dl->AddLine(P(0.0f, -0.32f), P(0.0f, 0.10f), u, T(1.0f));
            const ImVec2 head[] = {P(-0.17f, -0.06f), P(0.0f, 0.11f), P(0.17f, -0.06f)};
            dl->AddPolyline(head, 3, u, 0, T(1.0f));
            dl->AddLine(P(-0.30f, 0.30f), P(0.30f, 0.30f), u, T(1.0f));
            break;
        }
        case Glyph::Unsupported:
            dl->AddCircle(P(0.0f, 0.0f), size * 0.30f, u, 0, T(1.0f));
            dl->AddLine(P(-0.21f, -0.21f), P(0.21f, 0.21f), u, T(1.0f));
            break;
        case Glyph::Files: {
            const ImVec2 page[] = {P(-0.25f, -0.33f), P(0.08f, -0.33f), P(0.25f, -0.16f), P(0.25f, 0.33f),
                                   P(-0.25f, 0.33f)};
            dl->AddPolyline(page, 5, u, ImDrawFlags_Closed, T(1.0f));
            const ImVec2 fold[] = {P(0.08f, -0.33f), P(0.08f, -0.16f), P(0.25f, -0.16f)};
            dl->AddPolyline(fold, 3, u, 0, T(1.0f));
            break;
        }
        case Glyph::Image: {
            dl->AddRect(P(-0.34f, -0.26f), P(0.34f, 0.26f), u, size * 0.06f, 0, T(1.0f));
            if (filled) {
                dl->AddTriangleFilled(P(-0.24f, 0.17f), P(-0.04f, -0.07f), P(0.12f, 0.17f), u);
                dl->AddTriangleFilled(P(0.04f, 0.17f), P(0.15f, 0.03f), P(0.25f, 0.17f), u);
                dl->AddCircleFilled(P(0.15f, -0.12f), size * 0.05f, u);
            } else {
                const ImVec2 hills[] = {P(-0.24f, 0.17f), P(-0.04f, -0.07f), P(0.08f, 0.07f), P(0.15f, 0.0f),
                                        P(0.25f, 0.12f)};
                dl->AddPolyline(hills, 5, u, 0, T(1.0f));
                dl->AddCircle(P(0.15f, -0.12f), size * 0.05f, u, 0, T(1.0f));
            }
            break;
        }
        case Glyph::Video:
            dl->AddRect(P(-0.34f, -0.26f), P(0.34f, 0.26f), u, size * 0.06f, 0, T(1.0f));
            if (filled) {
                dl->AddTriangleFilled(P(-0.08f, -0.12f), P(-0.08f, 0.12f), P(0.13f, 0.0f), u);
            } else {
                const ImVec2 tri[] = {P(-0.08f, -0.12f), P(-0.08f, 0.12f), P(0.13f, 0.0f)};
                dl->AddPolyline(tri, 3, u, ImDrawFlags_Closed, T(1.0f));
            }
            break;
        case Glyph::Folder: {
            if (filled) {
                dl->AddRectFilled(P(-0.36f, -0.28f), P(-0.04f, -0.12f), u, size * 0.04f);
                dl->AddRectFilled(P(-0.36f, -0.20f), P(0.36f, 0.26f), u, size * 0.05f);
                break;
            }
            const ImVec2 outline[] = {P(-0.36f, -0.28f), P(-0.06f, -0.28f), P(0.02f, -0.18f), P(0.36f, -0.18f),
                                      P(0.36f, 0.26f),   P(-0.36f, 0.26f)};
            dl->AddPolyline(outline, 6, u, ImDrawFlags_Closed, T(1.0f));
            break;
        }
    }
}

}  // namespace mp::ui
