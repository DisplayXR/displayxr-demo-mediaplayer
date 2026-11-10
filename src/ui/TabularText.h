// SPDX-License-Identifier: Apache-2.0
//
// Tabular figures for ImGui's proportional UI font: every digit 0-9 is drawn centred in a
// cell as wide as the font's widest digit, so a label made of changing numbers (timecode,
// frame counters, percentages) keeps one width from frame to frame. Drawn with plain
// AddText, "TC 01:11:11:11" is narrower than "TC 00:00:00:00", and a label anchored by its
// right edge — or a box sized to its text — wiggles every frame as the digits tick.
#pragma once

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include <imgui.h>

#include <cmath>

namespace mp::ui {

inline bool IsTabularDigit(char c) { return c >= '0' && c <= '9'; }

// Width of the widest digit in the current font.
inline float TabularDigitWidth() {
    float w = 0.0f;
    for (char d = '0'; d <= '9'; d++) {
        const char ds[2] = {d, '\0'};
        const float dw = ImGui::CalcTextSize(ds).x;
        if (dw > w) w = dw;
    }
    return w;
}

// Size of `text` laid out with tabular digits (single line).
inline ImVec2 CalcTabularTextSize(const char* text) {
    const float dw = TabularDigitWidth();
    float x = 0.0f;
    const char* run = text;
    for (const char* p = text;; p++) {
        if (*p && !IsTabularDigit(*p)) continue;
        if (p > run) x += ImGui::CalcTextSize(run, p).x;
        if (!*p) break;
        x += dw;
        run = p + 1;
    }
    return ImVec2(x, ImGui::CalcTextSize(text).y);
}

// AddText with tabular digits: text runs at their natural width, each digit centred in its cell.
inline void AddTabularText(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text) {
    const float dw = TabularDigitWidth();
    float x = pos.x;
    const char* run = text;
    for (const char* p = text;; p++) {
        if (*p && !IsTabularDigit(*p)) continue;
        if (p > run) {
            dl->AddText(ImVec2(std::floor(x + 0.5f), pos.y), col, run, p);
            x += ImGui::CalcTextSize(run, p).x;
        }
        if (!*p) break;
        const float w = ImGui::CalcTextSize(p, p + 1).x;
        dl->AddText(ImVec2(std::floor(x + (dw - w) * 0.5f + 0.5f), pos.y), col, p, p + 1);
        x += dw;
        run = p + 1;
    }
}

}  // namespace mp::ui
#endif
