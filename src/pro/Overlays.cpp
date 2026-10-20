// SPDX-License-Identifier: Apache-2.0
#include "pro/Overlays.h"

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>

namespace mp::pro {

namespace {

struct Named {
    const char* name;
    float value;
};
constexpr Named kFrameLines[] = {{"Off", 0.0f},          {"2.39", 2.39f}, {"1.85", 1.85f}, {"16:9", 16.0f / 9.0f},
                                 {"9:16", 9.0f / 16.0f}, {"4:3", 4.0f / 3.0f}, {"1:1", 1.0f}};
constexpr Named kDesqueeze[] = {{"1.0x", 1.0f}, {"1.3x", 1.3f}, {"1.5x", 1.5f}, {"1.8x", 1.8f}, {"2.0x", 2.0f}};
constexpr int kFrameLineCount = (int)(sizeof(kFrameLines) / sizeof(kFrameLines[0]));
constexpr int kDesqueezeCount = (int)(sizeof(kDesqueeze) / sizeof(kDesqueeze[0]));

}  // namespace

int FrameLineCount() { return kFrameLineCount; }
const char* FrameLineName(int i) { return kFrameLines[std::clamp(i, 0, kFrameLineCount - 1)].name; }
float FrameLineAspect(int i) { return kFrameLines[std::clamp(i, 0, kFrameLineCount - 1)].value; }
int DesqueezeCount() { return kDesqueezeCount; }
const char* DesqueezeName(int i) { return kDesqueeze[std::clamp(i, 0, kDesqueezeCount - 1)].name; }
float DesqueezeFactor(int i) { return kDesqueeze[std::clamp(i, 0, kDesqueezeCount - 1)].value; }

void FrameLineRect(float pictureAspect, float lineAspect, float& x, float& y, float& w, float& h) {
    x = y = 0.0f;
    w = h = 1.0f;
    if (pictureAspect <= 0.0f || lineAspect <= 0.0f) return;
    if (lineAspect < pictureAspect) {  // narrower: pillar lines
        w = lineAspect / pictureAspect;
        x = 0.5f * (1.0f - w);
    } else {                           // wider: letterbox lines
        h = pictureAspect / lineAspect;
        y = 0.5f * (1.0f - h);
    }
}

void DrawOverlays(const OverlaySettings& s, const BurnIn& b, float px, float py, float pw, float ph, float panX,
                  float panY, float zoom) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (pw < 2.0f || ph < 2.0f) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // A rectangle given in picture fractions -> HUD pixels, through pan / zoom.
    auto toHud = [&](float u, float v) { return ImVec2(px + (u - panX) * zoom * pw, py + (v - panY) * zoom * ph); };
    const ImVec2 p0(px, py), p1(px + pw, py + ph);
    dl->PushClipRect(p0, p1, true);
    // Line weight. On a lenticular panel a thin vertical line crosses the slanted lens in
    // visible steps (panel check 2026-10-19: "the vertical yellow lines are a bit jaggy"),
    // so lines are at least ~3 px at a 1080-line picture and scale up from there.
    const float thin = std::max(2.0f, ph / 360.0f);

    if (s.frameLine > 0) {
        float fx, fy, fw, fh;
        FrameLineRect(pw / ph, FrameLineAspect(s.frameLine), fx, fy, fw, fh);
        const ImVec2 a = toHud(fx, fy), c = toHud(fx + fw, fy + fh);
        const ImU32 shade = IM_COL32(0, 0, 0, (int)(std::clamp(s.matte, 0.0f, 1.0f) * 255.0f));
        if (s.matte > 0.01f) {  // the four bands outside the line
            dl->AddRectFilled(p0, ImVec2(p1.x, a.y), shade);
            dl->AddRectFilled(ImVec2(p0.x, c.y), p1, shade);
            dl->AddRectFilled(ImVec2(p0.x, a.y), ImVec2(a.x, c.y), shade);
            dl->AddRectFilled(ImVec2(c.x, a.y), ImVec2(p1.x, c.y), shade);
        }
        dl->AddRect(a, c, IM_COL32(255, 210, 60, 230), 0.0f, 0, thin * 2.0f);
        dl->AddText(ImVec2(a.x + 6.0f, a.y + 4.0f), IM_COL32(255, 210, 60, 230), FrameLineName(s.frameLine));
    }
    if (s.safeAreas) {
        const float act = 0.93f, tit = 0.90f;
        dl->AddRect(toHud(0.5f - act / 2, 0.5f - act / 2), toHud(0.5f + act / 2, 0.5f + act / 2),
                    IM_COL32(230, 230, 230, 170), 0.0f, 0, thin);
        dl->AddRect(toHud(0.5f - tit / 2, 0.5f - tit / 2), toHud(0.5f + tit / 2, 0.5f + tit / 2),
                    IM_COL32(120, 200, 255, 190), 0.0f, 0, thin);
    }
    if (s.centreCross) {
        const ImVec2 m = toHud(0.5f, 0.5f);
        const float arm = ph * 0.025f;
        dl->AddLine(ImVec2(m.x - arm, m.y), ImVec2(m.x + arm, m.y), IM_COL32(255, 255, 255, 210), thin * 1.5f);
        dl->AddLine(ImVec2(m.x, m.y - arm), ImVec2(m.x, m.y + arm), IM_COL32(255, 255, 255, 210), thin * 1.5f);
    }
    dl->PopClipRect();

    // Burn-ins: fixed to the picture's top corners (not zoomed), like a monitor's.
    char left[160] = "", right[160] = "";
    int nl = 0, nr = 0;
    if (s.burnClip && !b.clip.empty()) nl += std::snprintf(left + nl, sizeof(left) - nl, "%s", b.clip.c_str());
    if (s.burnColour && !b.colour.empty())
        nl += std::snprintf(left + nl, sizeof(left) - nl, "%s%s", nl ? "   " : "", b.colour.c_str());
    if (s.burnTimecode && !b.timecode.empty())
        nr += std::snprintf(right + nr, sizeof(right) - nr, "TC %s", b.timecode.c_str());
    if (s.burnFrame && b.frame >= 0)
        nr += std::snprintf(right + nr, sizeof(right) - nr, "%sF %lld", nr ? "   " : "", b.frame);
    const float pad = 5.0f, inset = 10.0f;
    auto label = [&](const char* t, bool alignRight) {
        if (!t[0]) return;
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const float x = alignRight ? p1.x - std::max(inset, b.rightInset) - ts.x - 2 * pad
                                   : p0.x + std::max(inset, b.leftInset);
        const float y = p0.y + std::max(inset, b.topInset);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + ts.x + 2 * pad, y + ts.y + 2 * pad), IM_COL32(0, 0, 0, 170));
        dl->AddText(ImVec2(x + pad, y + pad), IM_COL32(255, 255, 255, 240), t);
    };
    label(left, false);
    label(right, true);
#else
    (void)s;
    (void)b;
    (void)px;
    (void)py;
    (void)pw;
    (void)ph;
    (void)panX;
    (void)panY;
    (void)zoom;
#endif
}

}  // namespace mp::pro
