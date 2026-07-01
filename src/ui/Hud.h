// SPDX-License-Identifier: Apache-2.0
//
// Hud (M2) — a minimal CPU text rasterizer for the window-space overlay. Draws a
// translucent panel sized tightly to the text, with antialiased white glyphs (from
// the embedded SF Mono atlas). The runtime composites the result as a window-space
// layer (XrCompositionLayerWindowSpaceEXT) into both eyes with a subtle parallax.
//
// The richer ImGui "subtle 3D" transport UI is a later milestone (PRD §9).
#pragma once

#include <cstdint>
#include <vector>

namespace mp::hud {

struct Rasterized {
    int width = 0;   // tight panel size actually used, in the top-left of the buffer
    int height = 0;
};

// Rasterize `text` ('\n' = newline) into `out` (resized to bufW*bufH*4, RGBA8). The
// panel is drawn tight to the text in the top-left; the rest is fully transparent.
// Returns the panel's pixel size so the caller can submit just that sub-rect.
Rasterized RenderText(std::vector<uint8_t>& out, int bufW, int bufH, const char* text);

} // namespace mp::hud
