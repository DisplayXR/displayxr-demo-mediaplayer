// SPDX-License-Identifier: Apache-2.0
//
// StereoGrid: the media grid in stereo (#180, docs/BROWSER.md "3D grid"). Under the
// "3D only" filter the panel goes to its 3D mode and every tile shows its left eye in
// view 0 and its right eye in view 1. The ImGui HUD stays the flat chrome (one image,
// disparity 0); the tiles are drawn into the projection atlas by the platform's renderer
// at the rects the HUD laid out THAT frame, from the frame description below.
//
// This file is the pure part: where the right eye's square is, the per-tile
// convergence shift and its clamp, the tile-rect to per-view quad math, and the
// rendering-mode borrow the browser holds while it shows. No ImGui, no Vulkan.
#pragma once

#include "media/StereoTypes.h"

#include <cstdint>
#include <vector>

namespace mp::browser {

// A rectangle in normalised coordinates ([0, 1] over an image, or over a view).
struct UvRect {
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    float W() const { return u1 - u0; }
    float H() const { return v1 - v0; }
};

// Whether `layout` packs both eyes into the thumbnail, so the tile can be stereo. Mono and
// Dual (a LIF or a two-track video, whose thumbnail is one view) are flat.
bool Packed(StereoLayout layout);

// A tile's atlas slot holds only the square it shows of the LEFT eye (TileCrop.h
// ShownRegion: bars off, centre square). The 3D grid puts the same square of the RIGHT eye
// (x, y, w, h of a w x h thumbnail) in a second slot, under this key.
inline int64_t RightEyeKey(int64_t id) { return -id - 2; }  // ids are >= 0; -1 is "none"
// The right eye's copy of the left eye's region (x, y, w, h) of a fullW x fullH thumbnail:
// the same place in the other half. False for an unpacked layout or a region that does not
// fit in the other half.
bool OtherEyeRegion(StereoLayout layout, uint32_t fullW, uint32_t fullH, uint32_t x, uint32_t y, uint32_t w,
                    uint32_t h, uint32_t& ox, uint32_t& oy);

// What the content check measured on a thumbnail's two eyes, as fractions of the eye's
// width with AutoConvergence's sign: x_left - x_right, + = crossed = in front of the plane.
struct DepthEstimate {
    bool ok = false;
    float subject = 0.0f;   // the subject (the nearest strong disparity mode)
    float nearest = 0.0f;   // the nearest content (95th percentile), >= subject
    float cropFrac = 1.0f;  // the tile's square's width over the eye's width
};

// How deep a tile sits. Amounts are in TILE widths, uncrossed (+ = behind the plane).
struct DepthPolicy {
    float subjectBehind = 0.0025f;  // where the subject goes: just behind the plane
    float maxBehind = 0.01f;        // the subject is pushed no further back than this
    float unmeasured = 0.005f;      // no estimate: bias the whole picture back by this
    float maxShift = 0.06f;         // a bad estimate never moves a crop further than this
};

// The uncrossed disparity to ADD to a tile, in tile widths: puts the subject at
// policy.subjectBehind, then pushes further (up to maxBehind for the subject) so the
// nearest content is not in front of the plane. `cropFrac` is the crop's width over the
// eye's width (a measured fraction of the eye becomes a fraction of the tile).
float TileShift(const DepthEstimate& e, float cropFrac, const DepthPolicy& policy);

// Move the two crops apart by `shift` tile widths: the left eye's picture moves left and
// the right eye's right by half each, adding `shift` of uncrossed disparity. Both crops
// stay inside their eye: when the shift would leave one, both zoom in about their centres
// by the same amount. False (crops unchanged) when no zoom makes room.
bool ApplyShift(UvRect& left, UvRect& right, const UvRect& leftEye, const UvRect& rightEye, float shift);

// A stereo tile's two crops, each normalised over its eye's slot image (the square the tile
// shows), moved apart by TileShift (`e` may be null: unmeasured). The slot holds only the
// square, so room for the shift comes from zooming in a little (ApplyShift). Returns the
// shift applied, in tile widths (0 when it did not fit).
float SlotCrops(const DepthEstimate* e, const DepthPolicy& policy, UvRect& left, UvRect& right);

// `r` (normalised over a slot's image) mapped into the page uv of the slot, uv0..uv1.
UvRect InSlot(const UvRect& r, float slotU0, float slotV0, float slotU1, float slotV1);

// One tile of the stereo grid, this frame. A flat tile has the same page and crop twice.
struct StereoTile {
    uint64_t texture[2] = {0, 0};  // the pages view 0 / view 1 sample (ImGui texture ids)
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // HUD px
    UvRect uv[2];                  // page uv: view 0's crop, view 1's crop
};

// What the renderer draws into the projection atlas this frame. The HUD fills each view's
// tile, so a HUD pixel maps to the same place in every view.
struct StereoFrame {
    bool active = false;
    float hudW = 0.0f, hudH = 0.0f;
    float clip[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // HUD px: the grid; nothing is drawn outside it
    float background[3] = {0.0f, 0.0f, 0.0f};  // display-referred: the HUD's window colour
    float fade = 0.0f;                          // HUD px: the flat border every tile fades to
    std::vector<StereoTile> tiles;
};

// A HUD-px rect as a quad in one view, normalised over the view's tile. False when it is
// empty or the HUD has no size.
bool ViewQuad(float x0, float y0, float x1, float y1, float hudW, float hudH, UvRect& out);
// A HUD-px clip as a scissor in an atlas of render tiles: tile (tx, ty) of renderW x
// renderH. False when nothing is left.
bool ViewScissor(const float clip[4], float hudW, float hudH, uint32_t tx, uint32_t ty, uint32_t renderW,
                 uint32_t renderH, int32_t& x, int32_t& y, uint32_t& w, uint32_t& h);
// Which eye view `view` of a `cols`-column tile grid shows: the player's rule, so a 2x2
// quad mode alternates left/right by column.
bool RightEye(uint32_t view, uint32_t cols);

// The rendering mode the browser wants: the 3D mode under the "3D only" filter, else the
// flat mode. -1 when the wanted mode does not exist and there is no flat one either.
int32_t BrowserMode(bool only3D, int32_t flat, int32_t stereo);

// The rendering mode the idle screen and the browser borrow, and hand back to the player.
// Borrow() is called whenever the wanted mode may have changed (each frame the browser
// shows is fine); it returns a mode to request, or -1. It asks once per change, asks again
// when the runtime lands on another mode, and asks for the mode already in force when a
// request it made for another one may still be pending. The mode in force when the first
// borrow began is the one Return() hands back, and only if the borrowed mode is still the
// one in force (the user may have changed it meanwhile).
class ModeBorrow {
public:
    int32_t Borrow(int32_t want, int32_t active);
    int32_t Return(int32_t active);
    bool Borrowing() const { return borrowed_ >= 0; }
    int32_t Borrowed() const { return borrowed_; }
    int32_t Before() const { return before_; }

private:
    int32_t before_ = -1;
    int32_t borrowed_ = -1;
    int32_t requested_ = -1;   // the last mode asked for, until the runtime changes mode
    int32_t lastActive_ = -1;
};

} // namespace mp::browser
