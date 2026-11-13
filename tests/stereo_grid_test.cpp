// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the 3D grid (#180, src/browser/StereoGrid.h, ThumbnailDepth.h,
// StereoPairs.h): where the right eye's square is, the per-tile convergence shift and its
// clamp, keeping a shifted crop inside its eye, the tile-rect to per-view quad math, the
// rendering-mode borrow that takes the panel to 3D under the "3D only" filter and hands it
// back, and the pairs made for one-view 3D photos (a two-view LIF, an MPO) at small size.

#include "browser/PairDiskCache.h"
#include "browser/StereoGrid.h"
#include "browser/StereoPairs.h"
#include "browser/ThumbnailDepth.h"
#include "media/ImageDecoder.h"
#include "media/LifLoader.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
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
// --- pairs for one-view 3D photos ------------------------------------------------------

void JpegSink(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    out->insert(out->end(), (const uint8_t*)data, (const uint8_t*)data + size);
}
std::vector<uint8_t> SolidJpeg(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) rgb[i * 3] = r, rgb[i * 3 + 1] = g, rgb[i * 3 + 2] = b;
    std::vector<uint8_t> out;
    stbi_write_jpg_to_func(&JpegSink, &out, w, h, 3, rgb.data(), 95);
    return out;
}
void PutU32BE(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back((uint8_t)(v >> s));
}
// A classic two-view LIF: the base JPEG (left), the right view as blob 1000001.
std::vector<uint8_t> TwoViewLif(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right) {
    const std::string json =
        R"({"views":[{"albedoId":-1,"xLocation":0.0},{"albedoId":1000001,"xLocation":1.0}]})";
    std::vector<uint8_t> lif = left;
    const size_t region = lif.size();
    PutU32BE(lif, 2);
    PutU32BE(lif, 7);
    PutU32BE(lif, (uint32_t)json.size());
    lif.insert(lif.end(), json.begin(), json.end());
    PutU32BE(lif, 1000001);
    PutU32BE(lif, (uint32_t)right.size());
    lif.insert(lif.end(), right.begin(), right.end());
    PutU32BE(lif, (uint32_t)(lif.size() + 6 - region));
    lif.push_back(0x1E);
    lif.push_back(0x1A);
    return lif;
}

void TestPairHelpers() {
    uint32_t ew = 0, eh = 0;
    PairEyeSize(400, 300, 0.5f, ew, eh);
    CHECK(ew == 200 && eh == 150, "eye at the view scale: %ux%u", ew, eh);
    PairEyeSize(400, 300, 2.0f, ew, eh);
    CHECK(ew == 400 && eh == 300, "never above the thumbnail");
    PairEyeSize(20, 10, 0.1f, ew, eh);
    CHECK(ew == 16 && eh == 10, "at least 16 px, within the thumbnail");

    std::vector<uint8_t> src(4 * 2 * 4, 0), out;
    for (int i = 0; i < 8; ++i) src[i * 4] = (uint8_t)(i % 2 ? 200 : 100);  // red 100,200,...
    ResizeRgba(src.data(), 4, 2, 2, 1, out);
    CHECK(out.size() == 8 && out[0] == 150 && out[4] == 150, "box filter averages (%u)", out[0]);

    std::vector<uint8_t> l(2 * 2 * 4, 10), r(2 * 2 * 4, 20), sbs;
    ComposePair(l, r, 2, 2, sbs);
    CHECK(sbs.size() == 32 && sbs[0] == 10 && sbs[8] == 20 && sbs[16] == 10 && sbs[31] == 20,
          "side by side, row by row");
}

// The pair of a two-view LIF, made from its right view decoded small and its one-view
// thumbnail, with fake descriptors and a full decode + box filter standing in for the
// platform's scaled decoder.
void TestStereoPairs() {
    const int W = 320, H = 200;
    const std::vector<uint8_t> lif = TwoViewLif(SolidJpeg(W, H, 220, 30, 30), SolidJpeg(W, H, 30, 30, 220));
    std::map<int, const std::vector<uint8_t>*> files{{7, &lif}};
    std::vector<int> closed;
    std::mutex closedMutex;
    int decodes = 0;
    StereoPairs pairs;
    CHECK(pairs.Initialize(
              [&](const uint8_t* d, size_t n, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
                  ++decodes;
                  const mp::DecodedImage img = mp::ImageDecoder::LoadFromMemory(d, n, "pair");
                  if (!img.Valid()) return false;
                  ResizeRgba(img.pixels.data(), (uint32_t)img.width, (uint32_t)img.height, w, h, rgba);
                  return true;
              },
              [&](int fd, uint64_t off, void* dst, size_t n) {
                  auto it = files.find(fd);
                  if (it == files.end() || off + n > it->second->size()) return false;
                  std::memcpy(dst, it->second->data() + off, n);
                  return true;
              },
              [&](int fd) {
                  std::lock_guard<std::mutex> lock(closedMutex);
                  closed.push_back(fd);
              }),
          "pair maker starts");
    // The one-view thumbnail: the left view at 160 x 100, red.
    std::vector<uint8_t> thumb((size_t)160 * 100 * 4);
    for (size_t i = 0; i < thumb.size(); i += 4) thumb[i] = 220, thumb[i + 1] = 30, thumb[i + 2] = 30, thumb[i + 3] = 255;
    pairs.Make(42, 1000, 7, lif.size(), thumb.data(), 160, 100, 0.75f);
    CHECK(pairs.Busy(42), "busy while it is made");
    std::vector<Thumbnail> made;
    std::vector<int64_t> failed;
    for (int i = 0; i < 400 && made.empty() && failed.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pairs.TakeResults(made, failed);
    }
    CHECK(made.size() == 1 && failed.empty(), "a pair is made");
    if (made.size() == 1) {
        const Thumbnail& t = made[0];
        CHECK(t.id == 42 && t.width == 240 && t.height == 75, "eyes at the view scale: %ux%u", t.width, t.height);
        const uint8_t* lp = &t.rgba[((size_t)37 * t.width + 60) * 4];
        const uint8_t* rp = &t.rgba[((size_t)37 * t.width + 180) * 4];
        CHECK(lp[0] > 150 && lp[2] < 100, "left eye: the thumbnail (red)");
        CHECK(rp[2] > 150 && rp[0] < 100, "right eye: the LIF's second view (blue)");
    }
    CHECK(decodes == 1, "only the right view is decoded (%d)", decodes);
    CHECK(!pairs.Busy(42) && pairs.Has(42, 1000) && !pairs.Has(42, 1001), "cached by id and modified time");
    Thumbnail again;
    CHECK(pairs.Cached(42, 1000, again) && again.width == 240, "the cache hands it back");
    {
        std::lock_guard<std::mutex> lock(closedMutex);
        CHECK(closed.size() == 1 && closed[0] == 7, "the descriptor is closed");
    }

    // A plain JPEG (no LIF trailer, no MP index) has no pair.
    const std::vector<uint8_t> plain = SolidJpeg(64, 64, 9, 9, 9);
    files[8] = &plain;
    pairs.Make(43, 1, 8, plain.size(), thumb.data(), 160, 100, 1.0f);
    made.clear();
    for (int i = 0; i < 400 && failed.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pairs.TakeResults(made, failed);
    }
    CHECK(failed.size() == 1 && failed[0] == 43 && made.empty(), "no second view: failed, flat");
    pairs.Shutdown();
    std::lock_guard<std::mutex> lock(closedMutex);
    CHECK(closed.size() == 2, "every descriptor closed (%zu)", closed.size());
}

// A LIF whose system thumbnail is NOT the whole view (the NP02J camera's, since 2026-07: a
// 512 x 512 EXIF thumbnail, a centre crop of the 16:10 view) must not get the whole right
// view squeezed into the crop's size. Thumbnails that keep the aspect (older LIFs, MPOs) take
// the old path unchanged.
void TestPairAspect() {
    const std::vector<uint8_t> jpeg = SolidJpeg(320, 200, 1, 2, 3);
    uint32_t w = 0, h = 0;
    CHECK(JpegSize(jpeg.data(), jpeg.size(), w, h) && w == 320 && h == 200, "JPEG size %ux%u", w, h);
    const uint8_t notJpeg[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!JpegSize(notJpeg, sizeof(notJpeg), w, h), "not a JPEG");

    // Older LIFs (no EXIF thumbnail): MediaStore keeps the view's aspect, to rounding.
    CHECK(ThumbnailIsWholeView(160, 100, 320, 200), "same aspect");
    CHECK(ThumbnailIsWholeView(512, 320, 4656, 2910), "16:10 photo, aspect kept");
    CHECK(ThumbnailIsWholeView(512, 288, 4656, 2619), "16:9 photo, aspect kept");
    CHECK(ThumbnailIsWholeView(288, 512, 2619, 4656), "portrait photo, aspect kept");
    CHECK(ThumbnailIsWholeView(512, 320, 0, 0), "view size unknown: as before");
    // The camera's square EXIF thumbnail of its 16:10 views (3264x2040, 4208x2630), and others.
    CHECK(!ThumbnailIsWholeView(512, 512, 4656, 2910), "square crop of 16:10");
    CHECK(!ThumbnailIsWholeView(512, 512, 4208, 2630), "camera 4208x2630, square EXIF thumbnail");
    CHECK(!ThumbnailIsWholeView(512, 512, 3264, 2040), "camera 3264x2040, square EXIF thumbnail");
    CHECK(!ThumbnailIsWholeView(512, 512, 2560, 1600), "square crop of 2560x1600");

    uint32_t ew = 0, eh = 0;
    PairEyeSizeOfView(512, 512, 4656, 2910, 0.75f, ew, eh);
    CHECK(ew == 614 && eh == 384, "eye at the view's aspect, covering 384 x 384: %ux%u", ew, eh);
    PairEyeSizeOfView(512, 512, 300, 200, 0.75f, ew, eh);
    CHECK(ew == 300 && eh == 200, "never above the view: %ux%u", ew, eh);

    // End to end: a two-view LIF (320 x 200 views, left red, right blue) whose thumbnail is a
    // 100 x 100 crop. Both eyes come from the file at the view's aspect.
    const std::vector<uint8_t> lif = TwoViewLif(SolidJpeg(320, 200, 220, 30, 30), SolidJpeg(320, 200, 30, 30, 220));
    int decodes = 0;
    StereoPairs pairs;
    CHECK(pairs.Initialize(
              [&](const uint8_t* d, size_t n, uint32_t dw, uint32_t dh, std::vector<uint8_t>& rgba) {
                  ++decodes;
                  const mp::DecodedImage img = mp::ImageDecoder::LoadFromMemory(d, n, "pair");
                  if (!img.Valid()) return false;
                  ResizeRgba(img.pixels.data(), (uint32_t)img.width, (uint32_t)img.height, dw, dh, rgba);
                  return true;
              },
              [&](int fd, uint64_t off, void* dst, size_t n) {
                  if (fd != 9 || off + n > lif.size()) return false;
                  std::memcpy(dst, lif.data() + off, n);
                  return true;
              },
              [](int) {}),
          "pair maker starts");
    std::vector<uint8_t> thumb((size_t)100 * 100 * 4);
    for (size_t i = 0; i < thumb.size(); i += 4) thumb[i] = 220, thumb[i + 1] = 30, thumb[i + 2] = 30, thumb[i + 3] = 255;
    pairs.Make(50, 1, 9, lif.size(), thumb.data(), 100, 100, 0.75f);
    std::vector<Thumbnail> made;
    std::vector<int64_t> failed;
    for (int i = 0; i < 400 && made.empty() && failed.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pairs.TakeResults(made, failed);
    }
    CHECK(made.size() == 1 && failed.empty(), "a pair is made");
    if (made.size() == 1) {
        const Thumbnail& t = made[0];
        // PairEyeSize(100, 100, 0.75) = 75 x 75, covered at 16:10: 120 x 75 per eye.
        CHECK(t.width == 240 && t.height == 75, "eyes at the view's aspect: %ux%u", t.width, t.height);
        const uint8_t* lp = &t.rgba[((size_t)37 * t.width + 60) * 4];
        const uint8_t* rp = &t.rgba[((size_t)37 * t.width + 180) * 4];
        CHECK(lp[0] > 150 && lp[2] < 100, "left eye: the LIF's first view (red)");
        CHECK(rp[2] > 150 && rp[0] < 100, "right eye: the LIF's second view (blue)");
    }
    CHECK(decodes == 2, "both views decoded when the thumbnail is a crop (%d)", decodes);
    pairs.Shutdown();
}

// A one-view LIF (one picture + a disparity map, what a 2D->3D conversion writes): the pair
// comes from the player's depth warp at the player's parallax. The picture is a grey field
// with two soft bright bars, one where the map says far (left half, 0) and one where it says
// near (right half, 255). The near bar must be crossed (left eye right of the right eye),
// the far bar uncrossed, each by half the parallax, and the edge between leaves no holes.
void PngSink(void* ctx, void* data, int size) { JpegSink(ctx, data, size); }

std::vector<uint8_t> OneViewLif(const std::vector<uint8_t>& base, const std::vector<uint8_t>& map,
                                const std::string& json) {
    std::vector<uint8_t> lif = base;
    const size_t region = lif.size();
    PutU32BE(lif, 2);
    PutU32BE(lif, 7);
    PutU32BE(lif, (uint32_t)json.size());
    lif.insert(lif.end(), json.begin(), json.end());
    PutU32BE(lif, 1000001);
    PutU32BE(lif, (uint32_t)map.size());
    lif.insert(lif.end(), map.begin(), map.end());
    PutU32BE(lif, (uint32_t)(lif.size() + 6 - region));
    lif.push_back(0x1E);
    lif.push_back(0x1A);
    return lif;
}

// The brightness-weighted centre of the bar around `x0` in row `y` of eye `eye` (0 / 1).
float BarCentre(const Thumbnail& t, int eye, int y, int x0, int half) {
    const int ew = (int)t.width / 2;
    double sum = 0, wsum = 0;
    for (int x = std::max(0, x0 - half); x < std::min(ew, x0 + half); ++x) {
        const double v = std::max(0, (int)t.rgba[((size_t)y * t.width + (size_t)(eye * ew + x)) * 4] - 100);
        sum += v * x;
        wsum += v;
    }
    return wsum > 0 ? (float)(sum / wsum) : -1.0f;
}

void TestOneViewPair() {
    const int W = 640, H = 400, farX = 160, nearX = 480;
    std::vector<uint8_t> rgb((size_t)W * H * 3), mapRgb((size_t)W * H * 3), invRgb((size_t)W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float df = (float)(x - farX), dn = (float)(x - nearX);
            const float bar = 150.0f * (std::exp(-df * df / 18.0f) + std::exp(-dn * dn / 18.0f));
            uint8_t* p = &rgb[((size_t)y * W + x) * 3];
            p[0] = p[1] = p[2] = (uint8_t)std::min(255.0f, 100.0f + bar);
            const uint8_t d = x < W / 2 ? 0 : 255;
            uint8_t* m = &mapRgb[((size_t)y * W + x) * 3];
            m[0] = m[1] = m[2] = d;
            uint8_t* n = &invRgb[((size_t)y * W + x) * 3];
            n[0] = n[1] = n[2] = (uint8_t)(255 - d);
        }
    std::vector<uint8_t> base, png, invPng;
    stbi_write_jpg_to_func(&JpegSink, &base, W, H, 3, rgb.data(), 98);
    stbi_write_png_to_func(&PngSink, &png, W, H, 3, mapRgb.data(), W * 3);
    stbi_write_png_to_func(&PngSink, &invPng, W, H, 3, invRgb.data(), W * 3);
    // Classic schema: gain 0.25 -> 1.5 % parallax. LIF 5.x: inv_z_map min < max (the map's
    // far end at 255, flipped), 1500 * 0.02 * (0.1 - 0.4) / 640 = -1.4 % -> 1.4 %.
    const std::vector<uint8_t> classic = OneViewLif(
        base, png, R"({"gain":0.25,"views":[{"albedoId":-1,"xLocation":0,"disparityId":1000001}]})");
    const std::vector<uint8_t> v5 = OneViewLif(
        base, invPng,
        R"({"stereo_render_data":{"baseline":0.02},"views":[{"image":{"blob_id":-1},"focal_px":1500,)"
        R"("width_px":640,"inv_z_map":{"blob_id":1000001,"min":0.1,"max":0.4}}]})");
    const std::vector<uint8_t> twoView = TwoViewLif(SolidJpeg(64, 40, 1, 2, 3), SolidJpeg(64, 40, 3, 2, 1));

    // Where things are, without decoding anything.
    {
        auto reader = [](const std::vector<uint8_t>& f) {
            return [&f](uint64_t off, void* dst, size_t n) {
                if (off + n > f.size()) return false;
                std::memcpy(dst, f.data() + off, n);
                return true;
            };
        };
        mp::LifLoader::DepthViewSpan one;
        CHECK(mp::LifLoader::FindDepthView(reader(classic), classic.size(), one), "classic one-view LIF found");
        CHECK(one.image.base && one.image.offset == 0 && one.image.size == base.size(), "image = the base JPEG");
        CHECK(one.mapSize == png.size() && !one.invert && Near(one.parallax, 0.015f, 1e-5f),
              "classic: gain 0.25 -> 1.5 %% (%.4f), map as is", one.parallax);
        CHECK(mp::LifLoader::FindDepthView(reader(v5), v5.size(), one), "5.x one-view LIF found");
        CHECK(one.invert && Near(one.parallax, 0.0140625f, 1e-5f), "5.x: min < max flips the map, %.4f",
              one.parallax);
        mp::LifLoader::ViewSpan l, r;
        CHECK(!mp::LifLoader::FindViews(reader(classic), classic.size(), l, r), "one view: no second view");
        CHECK(!mp::LifLoader::FindDepthView(reader(twoView), twoView.size(), one), "two views: not a depth view");
    }

    std::map<int, const std::vector<uint8_t>*> files{{11, &classic}, {12, &v5}};
    int decodes = 0;
    StereoPairs pairs;
    CHECK(pairs.Initialize(
              [&](const uint8_t* d, size_t n, uint32_t dw, uint32_t dh, std::vector<uint8_t>& out) {
                  ++decodes;
                  const mp::DecodedImage img = mp::ImageDecoder::LoadFromMemory(d, n, "pair");
                  if (!img.Valid()) return false;
                  ResizeRgba(img.pixels.data(), (uint32_t)img.width, (uint32_t)img.height, dw, dh, out);
                  return true;
              },
              [&](int fd, uint64_t off, void* dst, size_t n) {
                  auto it = files.find(fd);
                  if (it == files.end() || off + n > it->second->size()) return false;
                  std::memcpy(dst, it->second->data() + off, n);
                  return true;
              },
              [](int) {}),
          "pair maker starts");
    // The system's thumbnail: the picture itself (the whole view), at full size.
    const mp::DecodedImage thumb = mp::ImageDecoder::LoadFromMemory(base.data(), base.size(), "thumb");
    struct Case { int fd; float parallax; const char* name; };
    for (const Case& c : {Case{11, 0.015f, "classic"}, Case{12, 0.0140625f, "5.x"}}) {
        decodes = 0;
        pairs.Make(60 + c.fd, 1, c.fd, files[c.fd]->size(), thumb.pixels.data(), W, H, 1.0f);
        std::vector<Thumbnail> made;
        std::vector<int64_t> failed;
        for (int i = 0; i < 400 && made.empty() && failed.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            pairs.TakeResults(made, failed);
        }
        CHECK(made.size() == 1 && failed.empty(), "%s: a pair is made from the depth map", c.name);
        if (made.size() != 1) continue;
        const Thumbnail& t = made[0];
        CHECK(t.width == 2u * W && t.height == (uint32_t)H, "%s: eyes at the thumbnail's size %ux%u", c.name, t.width,
              t.height);
        CHECK(decodes == 1, "%s: only the map is decoded, the thumbnail is the left picture (%d)", c.name, decodes);
        // Disparity = left-eye x - right-eye x. DepthWarp: +parallax * W for the nearest
        // (255, crossed: in front), -parallax * W for the farthest, 0 at mid depth.
        const float want = c.parallax * (float)W;  // 9.6 / 9.0 px
        const int y = H / 2;
        const float nearD = BarCentre(t, 0, y, nearX, 16) - BarCentre(t, 1, y, nearX, 16);
        const float farD = BarCentre(t, 0, y, farX, 16) - BarCentre(t, 1, y, farX, 16);
        CHECK(Near(nearD, 0.5f * want, 0.35f), "%s: near bar crossed by %.2f px (want %.2f)", c.name, nearD,
              0.5f * want);
        CHECK(Near(farD, -0.5f * want, 0.35f), "%s: far bar uncrossed by %.2f px (want %.2f)", c.name, farD,
              -0.5f * want);
        // The right eye's near bar moved left of the picture's, its far bar right.
        CHECK(BarCentre(t, 1, y, nearX, 16) < (float)nearX && BarCentre(t, 1, y, farX, 16) > (float)farX,
              "%s: right eye shifted the right way", c.name);
        // No holes: the disocclusion at the depth edge and the rows' ends are filled from
        // the picture (grey >= ~100), never left black or transparent.
        bool filled = true;
        for (size_t i = 0; i < t.rgba.size(); i += 4)
            if (t.rgba[i + 3] != 255 || t.rgba[i] < 90) filled = false;
        CHECK(filled, "%s: holes filled", c.name);
    }
    pairs.Shutdown();
}

// --- the pairs kept on disk (PairDiskCache.h) ---------------------------------------------

std::string TempDir(const char* name) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::temp_directory_path(ec) / name;
    std::filesystem::remove_all(p, ec);
    return p.string();
}

void TestPairDiskKey() {
    const std::string k = PairDiskCache::Key(42, 1000, 5000, 512, 320, 0.75f);
    CHECK(k.size() == 16 && k.find_first_not_of("0123456789abcdef") == std::string::npos, "16 hex digits: %s",
          k.c_str());
    CHECK(k == PairDiskCache::Key(42, 1000, 5000, 512, 320, 0.75f), "stable");
    CHECK(k != PairDiskCache::Key(43, 1000, 5000, 512, 320, 0.75f), "another file");
    CHECK(k != PairDiskCache::Key(42, 1001, 5000, 512, 320, 0.75f), "modified time changed");
    CHECK(k != PairDiskCache::Key(42, 1000, 5001, 512, 320, 0.75f), "size changed");
    CHECK(k != PairDiskCache::Key(42, 1000, 5000, 512, 512, 0.75f), "another thumbnail size");
    CHECK(k != PairDiskCache::Key(42, 1000, 5000, 512, 320, 0.5f), "another eye scale");
    CHECK(k != PairDiskCache::Key(42, 1000, 5000, 512, 320, 0.75f, PairDiskCache::kPairFormatVersion + 1),
          "pair format version bumped");
}

void TestPairDiskLru() {
    const std::string dir = TempDir("mp_pair_disk_lru");
    std::vector<uint8_t> img((size_t)64 * 32 * 4);
    for (size_t i = 0; i < img.size(); ++i) img[i] = (uint8_t)((i * 37) % 251);  // noise: ~4+ KB each
    PairDiskCache c;
    CHECK(c.Initialize(dir, 1u << 20), "opens (creates) the directory");
    CHECK(c.Store("a", img.data(), 64, 32) && c.Store("b", img.data(), 64, 32) && c.Store("c", img.data(), 64, 32),
          "stores");
    const uint64_t one = c.Bytes() / 3;
    CHECK(c.Count() == 3 && one > 0, "three kept (%zu, %llu bytes each)", c.Count(), (unsigned long long)one);
    std::vector<uint8_t> jpeg;
    CHECK(c.Load("a", jpeg) && jpeg.size() > 2 && jpeg[0] == 0xFF && jpeg[1] == 0xD8, "a JPEG comes back");
    CHECK(!c.Load("zz", jpeg), "not stored: a miss");
    // Room for about three: storing a fourth trims to 90 %, the least recently used first.
    // "a" was just used, so "b" goes.
    c.Shutdown();
    CHECK(c.Initialize(dir, one * 3 + one / 2), "reopened: the index comes from the files");
    CHECK(c.Count() == 3, "three found on disk (%zu)", c.Count());
    CHECK(c.Load("a", jpeg), "a used");
    CHECK(c.Store("d", img.data(), 64, 32), "a fourth");
    CHECK(c.Bytes() <= one * 3 + one / 2, "within the budget");
    CHECK(c.Load("a", jpeg) && c.Load("d", jpeg), "the recently used stay");
    CHECK(!c.Load("b", jpeg), "the least recently used went");
    CHECK(!std::filesystem::exists(dir + "/b.jpg"), "its file too");
    // A write cut short (a temp file) is dropped when the cache opens.
    { std::ofstream(dir + "/x.tmp") << "partial"; }
    c.Shutdown();
    CHECK(c.Initialize(dir, 1u << 20) && !std::filesystem::exists(dir + "/x.tmp"), "stray temp file removed");
    c.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// End to end: a pair made once is read back from disk by the next launch (the file is not
// read again), and a corrupt stored pair is made again.
void TestPairDiskReuse() {
    const std::string dir = TempDir("mp_pair_disk_reuse");
    const std::vector<uint8_t> lif = TwoViewLif(SolidJpeg(320, 200, 220, 30, 30), SolidJpeg(320, 200, 30, 30, 220));
    std::atomic<int> reads{0};
    std::vector<uint8_t> thumb((size_t)160 * 100 * 4);
    for (size_t i = 0; i < thumb.size(); i += 4) thumb[i] = 220, thumb[i + 1] = 30, thumb[i + 2] = 30, thumb[i + 3] = 255;
    auto run = [&](Thumbnail& out) {
        StereoPairs pairs;
        pairs.Initialize(
            [](const uint8_t* d, size_t n, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
                const mp::DecodedImage img = mp::ImageDecoder::LoadFromMemory(d, n, "pair");
                if (!img.Valid()) return false;
                ResizeRgba(img.pixels.data(), (uint32_t)img.width, (uint32_t)img.height, w, h, rgba);
                return true;
            },
            [&](int, uint64_t off, void* dst, size_t n) {
                ++reads;
                if (off + n > lif.size()) return false;
                std::memcpy(dst, lif.data() + off, n);
                return true;
            },
            [](int) {}, dir);
        pairs.Make(77, 5, 3, lif.size(), thumb.data(), 160, 100, 0.75f);
        std::vector<Thumbnail> made;
        std::vector<int64_t> failed;
        for (int i = 0; i < 400 && made.empty() && failed.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            pairs.TakeResults(made, failed);
        }
        pairs.Shutdown();
        if (made.size() != 1) return false;
        out = std::move(made[0]);
        return true;
    };
    Thumbnail first, second, third;
    CHECK(run(first) && reads > 0, "first launch: made from the file");
    const std::string path = dir + "/" + PairDiskCache::Key(77, 5, lif.size(), 160, 100, 0.75f) + ".jpg";
    CHECK(std::filesystem::exists(path), "stored under its key");
    reads = 0;
    CHECK(run(second) && reads == 0, "second launch: from disk, the file not read (%d reads)", reads.load());
    CHECK(second.id == 77 && second.width == first.width && second.height == first.height, "same size %ux%u",
          second.width, second.height);
    if (second.rgba.size() == first.rgba.size() && !second.rgba.empty()) {
        const size_t mid = ((size_t)(second.height / 2) * second.width + second.width * 3 / 4) * 4;
        CHECK(second.rgba[mid + 2] > 150 && second.rgba[mid] < 100, "the right eye survived the JPEG (blue)");
    }
    { std::ofstream(path, std::ios::binary | std::ios::trunc) << "not a jpeg"; }
    reads = 0;
    CHECK(run(third) && reads > 0 && third.width == first.width, "corrupt on disk: made again");
    CHECK(std::filesystem::file_size(path) > 100, "and stored again");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

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
    TestPairHelpers();
    TestStereoPairs();
    TestPairAspect();
    TestOneViewPair();
    TestPairDiskKey();
    TestPairDiskLru();
    TestPairDiskReuse();
    if (g_failures) {
        std::fprintf(stderr, "stereo_grid_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("stereo_grid_test: all passed\n");
    return 0;
}
