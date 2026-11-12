// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the 3D grid (#180, src/browser/StereoGrid.h + ThumbnailDepth.h): each
// eye's crop of a thumbnail, the per-tile convergence shift and its clamp, keeping a
// shifted crop inside its eye, the tile-rect to per-view quad math, and the rendering-mode
// borrow that takes the panel to 3D under the "3D only" filter and hands it back.

#include "browser/StereoGrid.h"
#include "browser/ThumbnailDepth.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace mp::browser;
using mp::StereoLayout;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "  FAIL (%s:%d): ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                            \
            std::fprintf(stderr, "\n");                                   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool Inside(const UvRect& r, const UvRect& bounds) {
    const float e = 1e-5f;
    return r.u0 >= bounds.u0 - e && r.u1 <= bounds.u1 + e && r.v0 >= bounds.v0 - e && r.v1 <= bounds.v1 + e;
}

// Packed layouts, the right eye's key and where its square is.
void TestEyes() {
    CHECK(Packed(StereoLayout::SbsHalf) && Packed(StereoLayout::TbFull), "SBS and TB are packed");
    CHECK(!Packed(StereoLayout::Mono) && !Packed(StereoLayout::Dual), "mono and two-track are not");
    CHECK(RightEyeKey(0) < -1 && RightEyeKey(7) != RightEyeKey(8) && RightEyeKey(7) != 7, "right keys stay apart");

    // A 1024 x 288 full-SBS thumbnail: the tile's square of the left eye is 288 px at 112.
    uint32_t ox = 0, oy = 0;
    CHECK(OtherEyeRegion(StereoLayout::SbsFull, 1024, 288, 112, 0, 288, 288, ox, oy), "SBS: fits");
    CHECK(ox == 112 + 512 && oy == 0, "SBS: the same place in the right half (%u, %u)", ox, oy);
    CHECK(OtherEyeRegion(StereoLayout::TbHalf, 512, 576, 0, 16, 512, 256, ox, oy), "TB: fits");
    CHECK(ox == 0 && oy == 16 + 288, "TB: the same place in the bottom half (%u, %u)", ox, oy);
    CHECK(!OtherEyeRegion(StereoLayout::Mono, 512, 512, 0, 0, 512, 512, ox, oy), "mono has no other eye");
    CHECK(!OtherEyeRegion(StereoLayout::SbsFull, 1024, 288, 600, 0, 288, 288, ox, oy), "past the frame: refused");
}

// The shift: subject just behind the plane, the nearest content pulled back to the plane
// while the subject stays within 1 % of the tile, a bias back with no estimate, and a cap.
void TestTileShift() {
    const DepthPolicy p;
    DepthEstimate none;
    CHECK(Near(TileShift(none, 0.5f, p), p.unmeasured), "no estimate: biased back by %.4f", p.unmeasured);

    // The subject 2 % of the eye in front, the crop half the eye: 4 % of the tile.
    DepthEstimate e;
    e.ok = true;
    e.subject = 0.02f;
    e.nearest = 0.02f;
    float s = TileShift(e, 0.5f, p);
    CHECK(Near(s, 0.04f + p.subjectBehind), "subject to just behind: %.4f", s);
    CHECK(Near(s - 0.04f, p.subjectBehind), "the subject ends %.4f behind", s - 0.04f);

    // Nearer content 0.5 % of the tile in front of the subject: pushed to the plane.
    e.nearest = 0.02f + 0.005f * 0.5f;
    s = TileShift(e, 0.5f, p);
    CHECK(Near(s, 0.045f), "nearest content to the plane: %.4f", s);
    // ...but the subject never goes more than maxBehind back.
    e.nearest = 0.10f;
    s = TileShift(e, 0.5f, p);
    CHECK(Near(s - 0.04f, p.maxBehind), "subject stops at %.4f behind (got %.4f)", p.maxBehind, s - 0.04f);

    // A subject behind the plane is pulled forward to just behind it.
    e.subject = e.nearest = -0.01f;
    s = TileShift(e, 1.0f, p);
    CHECK(Near(s, -0.01f + p.subjectBehind), "subject behind: pulled forward, %.4f", s);

    // A wild estimate is capped.
    e.subject = e.nearest = 0.5f;
    CHECK(Near(TileShift(e, 0.5f, p), p.maxShift), "capped at %.3f", p.maxShift);
    e.subject = e.nearest = -0.5f;
    CHECK(Near(TileShift(e, 0.5f, p), -p.maxShift), "capped at -%.3f", p.maxShift);
}

// Applying it: the left crop moves right in uv (its picture moves left), the right crop
// left, by half the shift each, and both stay inside their eye.
void TestApplyShift() {
    const UvRect eyeL{0.0f, 0.0f, 0.5f, 1.0f}, eyeR{0.5f, 0.0f, 1.0f, 1.0f};
    const float half = 0.5f * 288.0f / 512.0f * 0.5f;  // a square crop of a 512 x 288 eye
    UvRect l{0.25f - half, 0.0f, 0.25f + half, 1.0f}, r{0.75f - half, 0.0f, 0.75f + half, 1.0f};
    const float w = l.W(), cl = (l.u0 + l.u1) * 0.5f, cr = (r.u0 + r.u1) * 0.5f;
    CHECK(ApplyShift(l, r, eyeL, eyeR, 0.02f), "room in a wide eye");
    CHECK(Near(l.W(), w) && Near(r.W(), w), "no zoom needed");
    CHECK(Near((l.u0 + l.u1) * 0.5f - cl, 0.01f * w), "left crop +shift/2 of its width");
    CHECK(Near((r.u0 + r.u1) * 0.5f - cr, -0.01f * w), "right crop -shift/2 of its width");
    // Displayed disparity added (uncrossed, in crop widths) = (left move - right move) / w.
    const float added = (((l.u0 + l.u1) * 0.5f - cl) - ((r.u0 + r.u1) * 0.5f - cr)) / w;
    CHECK(Near(added, 0.02f), "adds %.4f of the tile", added);

    // A crop that fills its eye has no room: both zoom in by the same amount, inside.
    UvRect fl = eyeL, fr = eyeR;
    CHECK(ApplyShift(fl, fr, eyeL, eyeR, 0.04f), "zooms to make room");
    CHECK(Inside(fl, eyeL) && Inside(fr, eyeR), "both crops inside their eye");
    CHECK(Near(fl.W(), fr.W()) && fl.W() < eyeL.W(), "same zoom for both");
    CHECK(Near(fl.W() / fl.H(), eyeL.W() / eyeL.H(), 1e-3f), "aspect kept");
    const float moved = (((fl.u0 + fl.u1) * 0.5f - 0.25f) - ((fr.u0 + fr.u1) * 0.5f - 0.75f)) / fl.W();
    CHECK(Near(moved, 0.04f), "still adds the asked disparity after the zoom (%.4f)", moved);

    UvRect bl = eyeL, br = eyeR;
    CHECK(!ApplyShift(bl, br, eyeL, eyeR, 0.9f), "an absurd shift is refused");
    CHECK(Near(bl.u0, eyeL.u0) && Near(br.u1, eyeR.u1), "...and leaves the crops alone");
}

// A tile's crops over its two slots: each slot holds exactly the square, so the shift
// zooms in a little and the crops move apart inside [0, 1].
void TestSlotCrops() {
    const DepthPolicy p;
    UvRect l, r;
    const float s = SlotCrops(nullptr, p, l, r);
    CHECK(Near(s, p.unmeasured), "unmeasured: biased back by %.4f", s);
    const UvRect whole;
    CHECK(Inside(l, whole) && Inside(r, whole), "inside the slots");
    CHECK((l.u0 + l.u1) * 0.5f > 0.5f && (r.u0 + r.u1) * 0.5f < 0.5f, "left crop right, right crop left");
    const float moved = (((l.u0 + l.u1) * 0.5f - 0.5f) - ((r.u0 + r.u1) * 0.5f - 0.5f)) / l.W();
    CHECK(Near(moved, s), "adds %.4f of the tile", moved);

    // A measured subject in front, the square a third of the eye: the subject's shift in
    // tile widths is three times its share of the eye.
    DepthEstimate e;
    e.ok = true;
    e.subject = e.nearest = 0.005f;
    e.cropFrac = 1.0f / 3.0f;
    CHECK(Near(SlotCrops(&e, p, l, r), 0.015f + p.subjectBehind), "measured: subject just behind");

    const UvRect m = InSlot(UvRect{0.0f, 0.0f, 0.5f, 1.0f}, 0.25f, 0.5f, 0.5f, 0.75f);
    CHECK(Near(m.u0, 0.25f) && Near(m.u1, 0.375f) && Near(m.v0, 0.5f) && Near(m.v1, 0.75f), "mapped into its slot");
}

// The tile at its HUD rect is the same normalised quad in every view; the clip becomes a
// scissor inside each view's tile of the atlas.
void TestViewMath() {
    UvRect q;
    CHECK(ViewQuad(100.0f, 50.0f, 300.0f, 250.0f, 1000.0f, 500.0f, q), "quad");
    CHECK(Near(q.u0, 0.1f) && Near(q.v0, 0.1f) && Near(q.u1, 0.3f) && Near(q.v1, 0.5f), "normalised over the HUD");
    CHECK(!ViewQuad(10.0f, 0.0f, 10.0f, 5.0f, 100.0f, 100.0f, q), "an empty rect has no quad");
    CHECK(!ViewQuad(0.0f, 0.0f, 5.0f, 5.0f, 0.0f, 100.0f, q), "no HUD, no quad");

    // A 1600 x 1000 HUD over 800 x 1000 tiles (SBS: viewScaleX 0.5), view 1 at tile (1, 0).
    const float clip[4] = {200.0f, 100.0f, 1600.0f, 1000.0f};
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    CHECK(ViewScissor(clip, 1600.0f, 1000.0f, 1, 0, 800, 1000, x, y, w, h), "scissor");
    CHECK(x == 900 && y == 100 && w == 700 && h == 900, "view 1 scissor %d,%d %ux%u", x, y, w, h);
    const float off[4] = {1700.0f, 0.0f, 1800.0f, 10.0f};
    CHECK(!ViewScissor(off, 1600.0f, 1000.0f, 0, 0, 800, 1000, x, y, w, h), "off the HUD: nothing");

    CHECK(!RightEye(0, 2) && RightEye(1, 2), "2x1: view 0 left, view 1 right");
    CHECK(!RightEye(2, 2) && RightEye(3, 2), "2x2 quad: by column");
    CHECK(!RightEye(0, 1) && RightEye(1, 1), "one column: by view");
}

// The browser's mode: the filter decides; there is no switch without a 3D mode.
void TestBrowserMode() {
    CHECK(BrowserMode(true, 0, 1) == 1, "3D only: the 3D mode");
    CHECK(BrowserMode(false, 0, 1) == 0, "otherwise flat");
    CHECK(BrowserMode(true, 0, -1) == 0, "no 3D mode: flat");
}

// The borrow, as the Android loop drives it: the splash borrows flat, the 3D filter turns
// the borrow into the 3D mode, leaving it borrows flat again, and the player gets back the
// mode it had.
void TestModeBorrow() {
    const int32_t flat = 0, stereo = 1;
    {
        // The player was in 3D. The browser opens (flat), the user picks 3D only, then a file.
        ModeBorrow b;
        CHECK(b.Borrow(flat, stereo) == flat, "splash: request flat");
        CHECK(b.Borrow(flat, stereo) == -1, "asked once, not every frame");
        CHECK(b.Borrow(flat, flat) == -1, "flat landed");
        CHECK(b.Borrow(stereo, flat) == stereo, "3D only: request 3D");
        CHECK(b.Borrow(stereo, flat) == -1, "...once, even while the switch is in flight");
        CHECK(b.Before() == stereo, "still hands back what the player had");
        CHECK(b.Return(stereo) == -1, "the player had 3D and 3D is in force: nothing to do");
        CHECK(!b.Borrowing(), "returned");
    }
    {
        // The player was flat: opening a file from the 3D grid hands flat back.
        ModeBorrow b;
        CHECK(b.Borrow(flat, flat) == -1, "already flat: nothing to request");
        CHECK(b.Borrowing() && b.Borrowed() == flat, "but it is a borrow");
        CHECK(b.Borrow(stereo, flat) == stereo, "3D only from a flat start");
        CHECK(b.Return(stereo) == flat, "back to flat for the player");
    }
    {
        // The filter changes back: flat again, and the player's 3D is handed back.
        ModeBorrow b;
        b.Borrow(flat, stereo);
        b.Borrow(stereo, flat);
        CHECK(b.Borrow(flat, stereo) == flat, "filter off: flat again");
        CHECK(b.Return(flat) == stereo, "the player's 3D back");
    }
    {
        // Back from the player into the 3D-filtered grid: the splash borrows flat, the next
        // frame wants 3D. The two land in one frame, so only the last request is issued.
        ModeBorrow b;
        CHECK(b.Borrow(flat, stereo) == flat, "back: splash flat");
        CHECK(b.Borrow(stereo, stereo) == stereo, "3D still in force: asked for over the pending flat");
        CHECK(b.Borrow(stereo, stereo) == -1, "...once");
        CHECK(b.Return(stereo) == -1, "and nothing to hand back");
    }
    {
        // The filter goes off while the switch to 3D is in flight, and the runtime then lands
        // on 3D anyway: flat is asked for again.
        ModeBorrow b;
        b.Borrow(flat, flat);
        CHECK(b.Borrow(stereo, flat) == stereo, "3D asked");
        CHECK(b.Borrow(flat, flat) == flat, "filter off before it landed: flat over it");
        CHECK(b.Borrow(flat, stereo) == flat, "landed on 3D anyway: flat again");
        CHECK(b.Borrow(flat, stereo) == -1, "...once");
    }
    {
        // The user changed the mode meanwhile: the borrow is not handed back over it.
        ModeBorrow b;
        b.Borrow(flat, stereo);
        CHECK(b.Return(2) == -1, "another mode in force: leave it");
    }
    {
        ModeBorrow b;
        CHECK(b.Borrow(-1, 0) == -1 && !b.Borrowing(), "no mode: no borrow");
        CHECK(b.Borrow(flat, -1) == -1 && !b.Borrowing(), "no active mode yet: no borrow");
    }
}

// The depth of a real-sized thumbnail: a textured SBS pair with the whole picture 8 px in
// front (eye 256 px: 3.1 %), measured on the worker.
void TestThumbnailDepth() {
    const int E = 256, H = 256, W = 2 * E, d = 8;
    std::vector<uint8_t> rgba((size_t)W * H * 4, 255);
    auto tex = [](int x, int y) {
        uint32_t h = (uint32_t)(x / 4) * 73856093u ^ (uint32_t)(y / 4) * 19349663u ^ 0x9e3779b9u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return (uint8_t)(40 + h % 176u);
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int xe = x < E ? x : x - E + d;  // the right eye sees the point x + d
            uint8_t* p = &rgba[((size_t)y * W + x) * 4];
            p[0] = p[1] = p[2] = tex(xe, y);
        }
    ThumbnailDepth depth;
    CHECK(depth.Initialize(), "worker starts");
    depth.Measure(7, rgba.data(), (uint32_t)W, (uint32_t)H, StereoLayout::SbsFull, 0.5f);
    depth.Measure(8, rgba.data(), (uint32_t)W, (uint32_t)H, StereoLayout::Mono, 1.0f);  // not packed: ignored
    DepthEstimate e;
    for (int i = 0; i < 400 && !depth.Get(7, e); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        depth.TakeResults();
    }
    CHECK(depth.Get(7, e) && e.ok, "measured");
    CHECK(Near(e.subject, (float)d / (float)E, 0.006f), "subject %.4f in front (want %.4f)", e.subject,
          (float)d / (float)E);
    CHECK(e.nearest >= e.subject - 1e-4f, "nearest is no further than the subject");
    CHECK(!depth.Get(8, e), "a mono thumbnail is not measured");
    depth.Forget(7);
    CHECK(!depth.Get(7, e), "forgotten");
    depth.Shutdown();
}

} // namespace

int main() {
    TestEyes();
    TestTileShift();
    TestApplyShift();
    TestSlotCrops();
    TestViewMath();
    TestBrowserMode();
    TestModeBorrow();
    TestThumbnailDepth();
    if (g_failures) {
        std::fprintf(stderr, "stereo_grid_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("stereo_grid_test: all passed\n");
    return 0;
}
