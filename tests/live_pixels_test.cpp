// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the capture-card pixel helpers (Media Player Pro): UYVY and BGRA to
// NV12, placing an eye at its column of a side-by-side frame, repeating an eye, and
// the --test-signal spec grammar. Pure: no SDK, no card, no GPU.

#include "pro/LivePixels.h"
#include "pro/TestSignalSource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using mp::livepix::Nv12;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// A 4x2 UYVY image: per row Cb Y0 Cr Y1 | Cb Y2 Cr Y3.
void UyvyBasics() {
    const uint8_t src[2][8] = {
        {100, 10, 200, 11, 110, 12, 210, 13},
        {102, 20, 202, 21, 112, 22, 212, 23},
    };
    Nv12 d;
    d.Resize(8, 2);  // a 2-eye frame, each eye 4 wide
    d.y.assign(d.y.size(), 0);
    d.uv.assign(d.uv.size(), 0);
    mp::livepix::UyvyToNv12(&src[0][0], 8, 4, 2, d, 4);  // the right eye
    CHECK(d.y[4] == 10 && d.y[5] == 11 && d.y[6] == 12 && d.y[7] == 13, "row 0 luma at x0=4");
    CHECK(d.y[12] == 20 && d.y[15] == 23, "row 1 luma at x0=4");
    CHECK(d.y[0] == 0 && d.y[3] == 0, "left eye untouched");
    // chroma averaged over the two rows: (100+102+1)/2 = 101, (200+202+1)/2 = 201
    CHECK(d.uv[4] == 101 && d.uv[5] == 201, "first chroma pair");
    CHECK(d.uv[6] == 111 && d.uv[7] == 211, "second chroma pair");
    CHECK(d.uv[0] == 0, "left eye chroma untouched");
}

void BgraGreyIsNeutral() {
    // Mid grey, full white and black: chroma must stay 128 and luma map to 16..235.
    std::vector<uint8_t> src(4 * 4 * 2);
    for (int i = 0; i < 8; ++i) {
        const uint8_t v = i < 4 ? 128 : (i < 6 ? 255 : 0);
        src[4 * i] = src[4 * i + 1] = src[4 * i + 2] = v;
        src[4 * i + 3] = 255;
    }
    Nv12 d;
    d.Resize(4, 2);
    mp::livepix::BgraToNv12(src.data(), 16, 4, 2, d, 0);
    CHECK(d.y[0] > 120 && d.y[0] < 130, "mid grey luma ~126");
    CHECK(d.y[4] == 235 || d.y[4] == 234, "white -> 235");
    CHECK(d.y[6] == 16, "black -> 16");
    CHECK(std::abs((int)d.uv[0] - 128) <= 1 && std::abs((int)d.uv[1] - 128) <= 1, "grey chroma neutral");
}

void BgraRedHasCr() {
    std::vector<uint8_t> src(4 * 2 * 2, 0);
    for (int i = 0; i < 4; ++i) {
        src[4 * i + 2] = 255;  // R
        src[4 * i + 3] = 255;
    }
    Nv12 d;
    d.Resize(2, 2);
    mp::livepix::BgraToNv12(src.data(), 8, 2, 2, d, 0);
    CHECK(d.uv[1] > 200, "red -> high Cr");
    CHECK(d.uv[0] < 128, "red -> low Cb");
}

void CopyEyeRepeats() {
    Nv12 d;
    d.Resize(8, 2);
    for (size_t i = 0; i < d.y.size(); ++i) d.y[i] = (uint8_t)i;
    for (size_t i = 0; i < d.uv.size(); ++i) d.uv[i] = (uint8_t)(100 + i);
    mp::livepix::CopyEye(d, 0, 4, 4);
    CHECK(d.y[4] == 0 && d.y[7] == 3 && d.y[12] == 8, "luma repeated");
    CHECK(d.uv[4] == 100 && d.uv[7] == 103, "chroma repeated");
}

void OutOfRangeIsIgnored() {
    Nv12 d;
    d.Resize(4, 2);
    d.y.assign(d.y.size(), 7);
    const uint8_t src[16] = {0};
    mp::livepix::UyvyToNv12(src, 8, 4, 2, d, 2);  // would run past the right edge
    CHECK(d.y[3] == 7, "an eye that does not fit is not written");
}

// v210: pack 6 pixels with known 10-bit values and unpack them.
void V210Basics() {
    // Cb0 Y0 Cr0 Y1 Cb2 Y2 Cr2 Y3 Cb4 Y4 Cr4 Y5
    const uint32_t cb[3] = {100, 200, 300}, cr[3] = {400, 500, 600};
    const uint32_t y[6] = {64, 128, 512, 700, 900, 940};
    uint32_t words[4] = {cb[0] | (y[0] << 10) | (cr[0] << 20), y[1] | (cb[1] << 10) | (y[2] << 20),
                         cr[1] | (y[3] << 10) | (cb[2] << 20), y[4] | (cr[2] << 10) | (y[5] << 20)};
    uint8_t row[2][128] = {};
    std::memcpy(row[0], words, 16);
    // second row: chroma +4 so the 4:2:0 average shows
    uint32_t words2[4] = {(cb[0] + 4) | (y[0] << 10) | ((cr[0] + 4) << 20), y[1] | ((cb[1] + 4) << 10) | (y[2] << 20),
                          (cr[1] + 4) | (y[3] << 10) | ((cb[2] + 4) << 20), y[4] | ((cr[2] + 4) << 10) | (y[5] << 20)};
    std::memcpy(row[1], words2, 16);
    mp::livepix::P016 d;
    d.Resize(12, 2);
    mp::livepix::V210ToP016(&row[0][0], 128, 6, 2, d, 6);
    CHECK(d.y[6] == (64 << 6) && d.y[11] == (940 << 6) && d.y[12 + 8] == (512 << 6), "v210 luma, MSB-aligned at x0=6");
    CHECK(d.uv[6] == ((100 + 2) << 6) && d.uv[7] == ((400 + 2) << 6), "first chroma pair averaged over 2 rows");
    CHECK(d.uv[10] == ((300 + 2) << 6) && d.uv[11] == ((600 + 2) << 6), "third chroma pair");
    CHECK(d.y[0] == 0, "left eye untouched");
}

// PR #108's vectors (its own v210 packer), as a cross-check of V210ToP016: six 10-bit
// lumas incl. both legal ends, chroma identical on both rows (no averaging), into x0=0.
void V210Pr108Vectors() {
    const int y[6] = {64, 128, 256, 512, 768, 1020}, cb[3] = {400, 500, 600}, cr[3] = {700, 800, 900};
    uint8_t row[2][128] = {};
    for (int r = 0; r < 2; ++r) {
        uint8_t* out = row[r];
        auto put = [&](int word, uint32_t v) { std::memcpy(out + word * 4, &v, 4); };
        put(0, (uint32_t)cb[0] | ((uint32_t)y[0] << 10) | ((uint32_t)cr[0] << 20));
        put(1, (uint32_t)y[1] | ((uint32_t)cb[1] << 10) | ((uint32_t)y[2] << 20));
        put(2, (uint32_t)cr[1] | ((uint32_t)y[3] << 10) | ((uint32_t)cb[2] << 20));
        put(3, (uint32_t)y[4] | ((uint32_t)cr[2] << 10) | ((uint32_t)y[5] << 20));
    }
    mp::livepix::P016 d;
    d.Resize(6, 2);
    mp::livepix::V210ToP016(&row[0][0], 128, 6, 2, d, 0);
    bool ok = true;
    for (int i = 0; i < 6; ++i) ok = ok && d.y[i] == (uint16_t)(y[i] << 6) && d.y[6 + i] == (uint16_t)(y[i] << 6);
    for (int i = 0; i < 3; ++i) ok = ok && d.uv[2 * i] == (uint16_t)(cb[i] << 6) && d.uv[2 * i + 1] == (uint16_t)(cr[i] << 6);
    CHECK(ok, "#108 v210 vectors: every luma and chroma sample kept at 10 bits");
}

// The fused v210 unpack (#141) against a per-sample reference, on random frames whose
// width is not a multiple of 6 or 48 (a partial last group, rows padded to 128 bytes),
// with an odd height (a last row with no chroma partner), placed at x0 = 6.
void V210MatchesReference() {
    uint32_t seed = 12345;
    auto rnd = [&] { return (seed = seed * 1664525u + 1013904223u) >> 8; };
    const int sizes[][2] = {{6, 2}, {8, 2}, {10, 3}, {12, 4}, {722, 7}, {1280, 6}, {1920, 4}};
    bool ok = true;
    for (const auto& sz : sizes) {
        const int w = sz[0], h = sz[1], rb = ((w + 47) / 48) * 128;
        std::vector<uint8_t> src((size_t)rb * h);
        for (uint8_t& b : src) b = (uint8_t)rnd();
        // sample k of row r (Cb Y Cr Y ... order), 10 bits
        auto sample = [&](int r, int k) {
            uint32_t word;
            std::memcpy(&word, src.data() + (size_t)r * rb + (size_t)(k / 3) * 4, 4);
            return (int)((word >> (10 * (k % 3))) & 0x3ff);
        };
        auto yAt = [&](int r, int x) { return sample(r, 2 * x + 1); };
        auto cAt = [&](int r, int pair, int cr) { return sample(r, 4 * pair + (cr ? 2 : 0)); };
        mp::livepix::P016 d;
        d.Resize(w + 12, h);
        std::fill(d.y.begin(), d.y.end(), (uint16_t)7);
        std::fill(d.uv.begin(), d.uv.end(), (uint16_t)7);
        mp::livepix::V210ToP016(src.data(), rb, w, h, d, 6);
        for (int r = 0; r < h; ++r)
            for (int x = 0; x < d.width; ++x) {
                const bool in = x >= 6 && x < 6 + w;
                ok = ok && d.y[(size_t)r * d.width + x] == (in ? (uint16_t)(yAt(r, x - 6) << 6) : 7);
            }
        for (int r = 0; r + 1 < h; r += 2)
            for (int p = 0; p < w / 2; ++p)
                for (int c = 0; c < 2; ++c) {
                    const int want = ((cAt(r, p, c) + cAt(r + 1, p, c) + 1) >> 1) << 6;
                    ok = ok && d.uv[(size_t)(r / 2) * d.width + 6 + 2 * p + c] == want;
                }
        ok = ok && d.uv[0] == 7 && d.uv[(size_t)d.width - 1] == 7;  // outside the eye: untouched
    }
    CHECK(ok, "v210: every luma and averaged chroma sample matches the per-sample reference");
}

// PR #108's UYVY-into-the-right-half vector (8-bit NV12 path).
void UyvyPr108Vector() {
    const uint8_t src[2][8] = {{10, 100, 20, 101, 30, 102, 40, 103}, {12, 110, 22, 111, 32, 112, 42, 113}};
    mp::livepix::Nv12 d;
    d.Resize(8, 2);
    mp::livepix::UyvyToNv12(&src[0][0], 8, 4, 2, d, 4);
    CHECK(d.y[0] == 0 && d.y[3] == 0, "#108: left half untouched");
    CHECK(d.y[4] == 100 && d.y[5] == 101 && d.y[7] == 103 && d.y[8 + 4] == 110 && d.y[8 + 7] == 113, "#108: luma");
    CHECK(d.uv[4] == 11 && d.uv[5] == 21 && d.uv[6] == 31 && d.uv[7] == 41, "#108: chroma averaged over the row pair");
}

void UyvyWidens() {
    const uint8_t src[2][8] = {{100, 10, 200, 11, 110, 12, 210, 13}, {102, 20, 202, 21, 112, 22, 212, 23}};
    mp::livepix::P016 d;
    d.Resize(4, 2);
    mp::livepix::UyvyToP016(&src[0][0], 8, 4, 2, d, 0);
    CHECK(d.y[0] == (10 << 8) && d.uv[0] == (101 << 8), "8-bit UYVY widened to 16 bits");
}

// The shader's Y'CbCr -> R'G'B' (shaders/sbs.frag YuvToRgb, 8-bit limited levels), per matrix.
void DecodeLimited(int y8, int cb8, int cr8, mp::ColorMatrix m, float rgb[3]) {
    const float y = (y8 - 16) / 219.0f, u = (cb8 - 128) / 224.0f, v = (cr8 - 128) / 224.0f;
    if (m == mp::ColorMatrix::BT601) {
        rgb[0] = y + 1.402f * v;
        rgb[1] = y - 0.344136f * u - 0.714136f * v;
        rgb[2] = y + 1.772f * u;
    } else if (m == mp::ColorMatrix::BT2020) {
        rgb[0] = y + 1.4746f * v;
        rgb[1] = y - 0.164553f * u - 0.571353f * v;
        rgb[2] = y + 1.8814f * u;
    } else {
        rgb[0] = y + 1.5748f * v;
        rgb[1] = y - 0.1873f * u - 0.4681f * v;
        rgb[2] = y + 1.8556f * u;
    }
}

// BgraToNv12 encodes SD with BT.601 and tags it so; decoding with the tag round-trips a
// saturated colour, decoding as BT.709 (the old hardcoded shader) visibly does not.
void BgraMatrixTagRoundTrips() {
    for (int h : {2, 720}) {
        std::vector<uint8_t> src((size_t)4 * 2 * h, 0);
        for (int i = 0; i < 2 * h; ++i) {
            src[4 * i + 2] = 255;  // R
            src[4 * i + 1] = 0;    // G
            src[4 * i + 0] = 0;    // B
            src[4 * i + 3] = 255;
        }
        Nv12 d;
        d.Resize(2, h);
        mp::livepix::BgraToNv12(src.data(), 8, 2, h, d, 0);
        const mp::ColorMatrix want = h >= 720 ? mp::ColorMatrix::BT709 : mp::ColorMatrix::BT601;
        CHECK(d.matrix == want, h >= 720 ? "HD BGRA tagged BT.709" : "SD BGRA tagged BT.601");
        float rgb[3];
        DecodeLimited(d.y[0], d.uv[0], d.uv[1], d.matrix, rgb);
        std::printf("  BGRA red, %d lines, %s -> R %.3f G %.3f B %.3f\n", h, mp::ColorMatrixName(d.matrix), rgb[0],
                    rgb[1], rgb[2]);
        CHECK(std::fabs(rgb[0] - 1.0f) < 0.02f && std::fabs(rgb[1]) < 0.02f && std::fabs(rgb[2]) < 0.02f,
              "red decodes back to red with the tagged matrix");
        if (h < 720) {
            float wrong[3];
            DecodeLimited(d.y[0], d.uv[0], d.uv[1], mp::ColorMatrix::BT709, wrong);
            CHECK(std::fabs(wrong[1]) > 0.05f, "and decoding SD as BT.709 would shift it (the old bug)");
        }
    }
    mp::livepix::P016 p;
    p.Resize(4, 2);
    const uint8_t uyvy[2][8] = {};
    mp::livepix::UyvyToP016(&uyvy[0][0], 8, 4, 2, p, 0);
    CHECK(p.matrix == mp::ColorMatrix::BT601, "SD UYVY carries the SDI rule (BT.601)");
}

void TestSignalSpec() {
    mp::TestSignalConfig c;
    CHECK(c.ParseSpec("") && c.eyeW == 1920 && c.fps == 60.0, "empty spec keeps defaults");
    CHECK(c.ParseSpec("3840x2160@24") && c.eyeW == 3840 && c.eyeH == 2160 && c.fps == 24.0, "full spec");
    mp::TestSignalConfig d;
    CHECK(d.ParseSpec("1280x720") && d.eyeW == 1280 && d.fps == 60.0, "size only");
    mp::TestSignalConfig e;
    CHECK(e.ParseSpec("@30") && e.eyeW == 1920 && e.fps == 30.0, "rate only");
    mp::TestSignalConfig f;
    CHECK(!f.ParseSpec("big"), "garbage refused");
    CHECK(!f.ParseSpec("10x10"), "too small refused");
    CHECK(!f.ParseSpec("1920x1080@0"), "zero rate refused");
    mp::TestSignalConfig g;
    CHECK(g.ParseSpec("1280x720@30,image=C:/shots/cup, 2x1.jpg") && g.eyeW == 1280 && g.fps == 30.0 &&
              g.image == "C:/shots/cup, 2x1.jpg", "photo rig: size, rate and a path with a comma");
    mp::TestSignalConfig h;
    CHECK(h.ParseSpec("image=a.png") && h.image == "a.png" && h.eyeW == 1920, "photo rig: path only");
    CHECK(!h.ParseSpec("image="), "photo rig: empty path refused");
}

}  // namespace

int main() {
    UyvyBasics();
    BgraGreyIsNeutral();
    BgraRedHasCr();
    CopyEyeRepeats();
    OutOfRangeIsIgnored();
    TestSignalSpec();
    V210Basics();
    UyvyWidens();
    V210Pr108Vectors();
    V210MatchesReference();
    UyvyPr108Vector();
    BgraMatrixTagRoundTrips();
    if (g_failures) {
        std::fprintf(stderr, "live_pixels_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("live_pixels_test: all passed\n");
    return 0;
}
