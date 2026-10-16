// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the capture-card pixel helpers (Media Player Pro): UYVY and BGRA to
// NV12, placing an eye at its column of a side-by-side frame, repeating an eye, and
// the --test-signal spec grammar. Pure: no SDK, no card, no GPU.

#include "pro/LivePixels.h"
#include "pro/TestSignalSource.h"

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

void UyvyWidens() {
    const uint8_t src[2][8] = {{100, 10, 200, 11, 110, 12, 210, 13}, {102, 20, 202, 21, 112, 22, 212, 23}};
    mp::livepix::P016 d;
    d.Resize(4, 2);
    mp::livepix::UyvyToP016(&src[0][0], 8, 4, 2, d, 0);
    CHECK(d.y[0] == (10 << 8) && d.uv[0] == (101 << 8), "8-bit UYVY widened to 16 bits");
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
    if (g_failures) {
        std::fprintf(stderr, "live_pixels_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("live_pixels_test: all passed\n");
    return 0;
}
