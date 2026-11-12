// SPDX-License-Identifier: Apache-2.0
#include "ui/UiText.h"

#include "imgui.h"

namespace mp::ui {

std::string FitText(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const char* kEllipsis = "\xE2\x80\xA6";
    const float ellipsis = ImGui::CalcTextSize(kEllipsis).x;
    size_t n = text.size();
    while (n > 0 && ImGui::CalcTextSize(text.c_str(), text.c_str() + n).x + ellipsis > width) {
        --n;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) --n;  // whole UTF-8 characters
    }
    return text.substr(0, n) + kEllipsis;
}

std::vector<std::string> WrapText(const std::string& text, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        const size_t space = text.find(' ', i);
        const size_t end = space == std::string::npos ? text.size() : space;
        const std::string word = text.substr(i, end - i);
        const std::string joined = line.empty() ? word : line + " " + word;
        if (line.empty() || ImGui::CalcTextSize(joined.c_str()).x <= width) {
            line = joined;
        } else {
            lines.push_back(FitText(line, width));
            line = word;
        }
        i = end == text.size() ? end : end + 1;
    }
    if (!line.empty()) lines.push_back(FitText(line, width));
    return lines;
}

}  // namespace mp::ui
