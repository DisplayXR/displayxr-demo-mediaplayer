// SPDX-License-Identifier: Apache-2.0
//
// UiText: fitting text into a width in the current ImGui font, shared by the player's
// chrome and the browser (#180). Render thread, between NewFrame and Render.
#pragma once

#include <string>
#include <vector>

namespace mp::ui {

// `text` cut to fit in `width`, ending in an ellipsis if it was cut. Whole UTF-8 characters.
std::string FitText(const std::string& text, float width);

// `text` broken into lines no wider than `width`, at spaces. A word wider than the width is
// cut to fit (FitText). Never empty for a non-empty text.
std::vector<std::string> WrapText(const std::string& text, float width);

}  // namespace mp::ui
