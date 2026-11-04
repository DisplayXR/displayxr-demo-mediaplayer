// SPDX-License-Identifier: Apache-2.0
//
// Icons: the media browser's icons, hand-drawn with ImDrawList like TransportUI's, so no
// icon font is needed and they stay sharp at any size.
#pragma once

#include "imgui.h"

namespace mp::browser {

enum class Icon {
    Menu,         // three bars: show the sources
    More,         // three dots: the view options
    AllMedia,     // four tiles
    Camera,
    Download,
    Unsupported,  // a barred circle
    Files,        // a document: "Other files..."
    Image,        // Pictures, and a tile with no thumbnail
    Video,        // Movies, and a video tile with no thumbnail
};

// Draw `icon` to fit a `size` x `size` box centred on `center`.
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color);

} // namespace mp::browser
