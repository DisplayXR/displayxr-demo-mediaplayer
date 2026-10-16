// SPDX-License-Identifier: Apache-2.0
// Unit tests for the hardware-free half of the Pro DeckLink input: UYVY / v210 ->
// NV12 conversion (including placement into one half of a side-by-side frame) and
// the two-input stereo pairer.
#include "pro/DeckLinkFrames.h"

#include <cstdio>
#include <vector>

using namespace mp::pro::dl;

static int g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

// Pack one v210 row from 10-bit Y and 4:2:2 Cb/Cr samples (w a multiple of 6).
static void PackV210Row(const std::vector<int>& y, const std::vector<int>& cb, const std::vector<int>& cr,
                        uint8_t* out) {
    auto put = [&](int word, uint32_t v) {
        out[word * 4 + 0] = (uint8_t)v;
        out[word * 4 + 1] = (uint8_t)(v >> 8);
        out[word * 4 + 2] = (uint8_t)(v >> 16);
        out[word * 4 + 3] = (uint8_t)(v >> 24);
    };
    const int groups = (int)y.size() / 6;
    for (int g = 0; g < groups; ++g) {
        const int Y = g * 6, C = g * 3;
        put(g * 4 + 0, (uint32_t)cb[C] | ((uint32_t)y[Y] << 10) | ((uint32_t)cr[C] << 20));
        put(g * 4 + 1, (uint32_t)y[Y + 1] | ((uint32_t)cb[C + 1] << 10) | ((uint32_t)y[Y + 2] << 20));
        put(g * 4 + 2, (uint32_t)cr[C + 1] | ((uint32_t)y[Y + 3] << 10) | ((uint32_t)cb[C + 2] << 20));
        put(g * 4 + 3, (uint32_t)y[Y + 4] | ((uint32_t)cr[C + 2] << 10) | ((uint32_t)y[Y + 5] << 20));
    }
}

static void TestUyvyIntoRightHalf() {
    // 4x2 UYVY source placed at x=4 of an 8-wide NV12 frame (the right eye of SBS).
    const int w = 4, h = 2, stride = 8;
    const uint8_t src[2][8] = {{10, 100, 20, 101, 30, 102, 40, 103},
                               {12, 110, 22, 111, 32, 112, 42, 113}};
    std::vector<uint8_t> Y(stride * h, 0), UV(stride * (h / 2), 0);
    ToNV12(SrcFormat::UYVY8, &src[0][0], 8, w, h, Y.data(), UV.data(), stride, 4);
    CHECK(Y[0] == 0 && Y[3] == 0);                   // left half untouched
    CHECK(Y[4] == 100 && Y[5] == 101 && Y[7] == 103);
    CHECK(Y[stride + 4] == 110 && Y[stride + 7] == 113);
    CHECK(UV[4] == 11 && UV[5] == 21);               // (10+12)/2, (20+22)/2
    CHECK(UV[6] == 31 && UV[7] == 41);
}

static void TestV210() {
    const int w = 6, h = 2;
    std::vector<int> y = {64, 128, 256, 512, 768, 1020};
    std::vector<int> cb = {400, 500, 600}, cr = {700, 800, 900};
    const int rb = V210RowBytes(w);
    CHECK(rb == 128);
    std::vector<uint8_t> src(rb * h, 0);
    PackV210Row(y, cb, cr, src.data());
    PackV210Row(y, cb, cr, src.data() + rb);
    std::vector<uint8_t> Y(w * h), UV(w * (h / 2));
    ToNV12(SrcFormat::V210, src.data(), rb, w, h, Y.data(), UV.data(), w, 0);
    for (int i = 0; i < 6; ++i) CHECK(Y[i] == (uint8_t)(y[i] >> 2));
    for (int i = 0; i < 3; ++i) {
        CHECK(UV[2 * i] == (uint8_t)(cb[i] >> 2));
        CHECK(UV[2 * i + 1] == (uint8_t)(cr[i] >> 2));
    }
}

static void TestPairer() {
    StereoPairer p(500);  // half a frame at 1000 units/frame
    CHECK(!p.Offer(0, 1000));
    CHECK(p.Offer(1, 1100));         // within tolerance -> pair
    CHECK(p.GetStats().pairs == 1 && p.GetStats().lastSkew == 100);
    CHECK(!p.Offer(1, 2050));        // right first this time
    CHECK(p.Offer(0, 2000));
    CHECK(p.GetStats().pairs == 2);
    // Left runs a frame ahead with no matching right: the stale one is dropped.
    CHECK(!p.Offer(0, 3000));
    CHECK(!p.Offer(1, 4100));        // 1100 apart: left 3000 dropped, right 4100 waits
    CHECK(p.GetStats().unpaired == 1);
    CHECK(p.Offer(0, 4000));         // pairs with the waiting right frame
    CHECK(p.GetStats().pairs == 3 && p.GetStats().maxAbsSkew == 100);
    // Same eye twice in a row: the first never found a partner.
    CHECK(!p.Offer(0, 5000));
    CHECK(!p.Offer(0, 6000));
    CHECK(p.GetStats().unpaired == 2);
}

int main() {
    TestUyvyIntoRightHalf();
    TestV210();
    TestPairer();
    if (g_fail) {
        std::fprintf(stderr, "decklink_frames_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("decklink_frames_test: all passed\n");
    return 0;
}
