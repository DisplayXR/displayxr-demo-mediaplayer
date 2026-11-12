// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for FindActiveArea (media/Letterbox.h, #180): black bars come off a
// letterboxed or pillarboxed picture, and nothing comes off a picture that merely has dark
// parts (a night sky, a vignette, a black frame, one dark band, bars too thin to matter).
// Synthetic images, no fixtures.

#include "media/Letterbox.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using mp::ActiveArea;
using mp::FindActiveArea;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    Image(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 4, 255) {}
    void Set(int x, int y, uint8_t v) {
        uint8_t* p = &px[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = v;
    }
    // A textured picture over [x0, x1) x [y0, y1), everything else `bar`.
    void Picture(int x0, int y0, int x1, int y1, uint8_t bar = 0) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
                Set(x, y, in ? (uint8_t)(60 + (x * 7 + y * 13) % 150) : bar);
            }
        }
    }
    bool Find(ActiveArea& a) const { return FindActiveArea(px.data(), w, h, (ptrdiff_t)w * 4, a); }
};

bool Is(const ActiveArea& a, int x, int y, int w, int h) { return a.x == x && a.y == y && a.w == w && a.h == h; }

void TestNoBars() {
    Image img(400, 225);
    img.Picture(0, 0, 400, 225);
    ActiveArea a;
    CHECK(!img.Find(a) && Is(a, 0, 0, 400, 225), "a full picture keeps everything");
}

void TestLetterbox() {
    // 2.39:1 in 16:9: 400 x 167 of picture in 400 x 225, bars of 29.
    Image img(400, 225);
    img.Picture(0, 29, 400, 196);
    ActiveArea a;
    CHECK(img.Find(a) && Is(a, 0, 29, 400, 167), "letterbox bars come off");

    // Bars that differ by a line (an odd remainder) still pair up.
    img.Picture(0, 29, 400, 195);
    CHECK(img.Find(a) && Is(a, 0, 29, 400, 166), "bars a line apart still count");

    // Near black is black: an encoder's 16-ish bars.
    img.Picture(0, 29, 400, 196, 12);
    CHECK(img.Find(a) && Is(a, 0, 29, 400, 167), "near-black bars come off");

    // A scaled-down thumbnail blurs the edge over a line.
    img.Picture(0, 29, 400, 196);
    for (int x = 0; x < 400; ++x) {
        img.Set(x, 29, 30);
        img.Set(x, 195, 30);
    }
    CHECK(img.Find(a) && a.y >= 29 && a.y <= 30 && a.h >= 165 && a.h <= 167, "a soft edge is still an edge");
}

void TestPillarbox() {
    // 4:3 in 16:9: 300 x 225 in 400 x 225.
    Image img(400, 225);
    img.Picture(50, 0, 350, 225);
    ActiveArea a;
    CHECK(img.Find(a) && Is(a, 50, 0, 300, 225), "pillarbox bars come off");

    // Both at once (a windowbox).
    img.Picture(50, 20, 350, 205);
    CHECK(img.Find(a) && Is(a, 50, 20, 300, 185), "letterbox and pillarbox together");
}

void TestDarkPictures() {
    ActiveArea a;
    Image img(400, 225);

    img.Picture(0, 0, 0, 0);
    CHECK(!img.Find(a) && Is(a, 0, 0, 400, 225), "a black frame is not cropped");

    // A night sky: one dark band at the top only.
    img.Picture(0, 60, 400, 225);
    CHECK(!img.Find(a), "one dark band is not a letterbox");

    // Dark bands of very different sizes.
    img.Picture(0, 60, 400, 210);
    CHECK(!img.Find(a), "unequal dark bands are not a letterbox");

    // A vignette: dark at both ends, fading in over many lines.
    img.Picture(0, 0, 400, 225);
    for (int y = 0; y < 225; ++y) {
        const int d = y < 112 ? y : 224 - y;  // lines from the nearer edge
        const float k = d >= 60 ? 1.0f : (float)d / 60.0f;
        for (int x = 0; x < 400; ++x) {
            const uint8_t* p = &img.px[((size_t)y * 400 + x) * 4];
            img.Set(x, y, (uint8_t)((float)p[0] * k * k));
        }
    }
    CHECK(!img.Find(a), "a vignette fades, it does not stop: no crop");

    // Bars too thin to be bars (a 1 px dark line at both ends).
    img.Picture(0, 1, 400, 224);
    CHECK(!img.Find(a), "1 px edges are not bars");

    // Bars so big hardly any picture is left: a mostly dark frame.
    img.Picture(0, 90, 400, 135);
    CHECK(!img.Find(a), "a thin strip of light in the dark is not a letterbox");
}

void TestSubRectangle() {
    // A side-by-side frame whose left eye is pillarboxed: measure the left half alone.
    Image img(800, 225);
    img.Picture(0, 0, 800, 225);
    for (int y = 0; y < 225; ++y) {
        for (int x = 0; x < 800; ++x) {
            const int ex = x % 400;
            if (ex < 50 || ex >= 350) img.Set(x, y, 0);
        }
    }
    ActiveArea a;
    CHECK(FindActiveArea(img.px.data(), 400, 225, (ptrdiff_t)800 * 4, a) && Is(a, 50, 0, 300, 225),
          "the left eye of a stereo frame, through the row stride");
    CHECK(!FindActiveArea(nullptr, 400, 225, 1600, a) && !FindActiveArea(img.px.data(), 0, 225, 1600, a),
          "no image, no crop");
}

}  // namespace

int main() {
    TestNoBars();
    TestLetterbox();
    TestPillarbox();
    TestDarkPictures();
    TestSubRectangle();
    if (g_failures) {
        std::fprintf(stderr, "letterbox_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("letterbox_test: all passed\n");
    return 0;
}
