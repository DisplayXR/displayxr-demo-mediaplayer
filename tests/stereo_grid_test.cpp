// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the 3D grid (#180, src/browser/StereoGrid.h, ThumbnailDepth.h,
// StereoPairs.h): where the right eye's square is, the per-tile convergence shift and its
// clamp, keeping a shifted crop inside its eye, the tile-rect to per-view quad math, the
// rendering-mode borrow that takes the panel to 3D under the "3D only" filter and hands it
// back, and the pairs made for one-view 3D photos (a two-view LIF, an MPO) at small size.

#include "browser/StereoGrid.h"
#include "browser/StereoPairs.h"
#include "browser/ThumbnailDepth.h"
#include "media/ImageDecoder.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    if (g_failures) {
        std::fprintf(stderr, "stereo_grid_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("stereo_grid_test: all passed\n");
    return 0;
}
