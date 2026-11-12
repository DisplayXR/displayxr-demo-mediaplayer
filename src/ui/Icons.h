// SPDX-License-Identifier: Apache-2.0
//
// Icons: every icon the player and the browser draw, as one set (#180). Hand-drawn with
// ImDrawList, so no icon font is needed and they stay sharp at any size. One look: outline
// glyphs, filled only for an active state (the source in force; play and pause are solid
// by convention).
//
// One stroke for all of them. On touch it is g_iconStroke (2 dp, set by the Android leg).
// On desktop (g_iconStroke = 0) a stroke is a fixed fraction of the glyph's box, with the
// per-part weights the player's icons have always had, so the desktop draws exactly the
// pixels it drew before the two hand-drawn sets were merged.
#pragma once

#include "imgui.h"

namespace mp::ui {

enum class Glyph {
    // The player's transport.
    Play,
    Pause,
    Loop,
    Slideshow,
    Speaker,
    SpeakerMuted,
    SkipBack,     // an arrow turning back round "10"
    SkipForward,  // an arrow turning forward round "10"
    Info,         // a circled "i"
    // Navigation.
    Back,         // a left arrow: the player's way back to the browser, a folder up
    Menu,         // three bars: the sources
    More,         // three dots: the view options
    Close,        // a cross: leave select mode
    Delete,       // a bin
    // Sources and tiles.
    AllMedia,     // four tiles
    Camera,
    Download,
    Unsupported,  // a barred circle
    Files,        // a document: "Other files..."
    Image,        // Pictures, and a tile with no thumbnail
    Video,        // Movies, and a video tile with no thumbnail
    Folder,
};

// One stroke width for every outline, HUD pixels; 0 = the desktop's proportional strokes.
inline float g_iconStroke = 0.0f;

// Draw `glyph` in a `box` x `box` square centred on `center`; the glyph spans about 60% of
// it. `filled`: the active state, for a glyph that has one (a source in force).
void DrawGlyph(ImDrawList* dl, Glyph glyph, ImVec2 center, float box, ImU32 color, bool filled = false);

}  // namespace mp::ui
