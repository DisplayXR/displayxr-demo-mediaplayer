// SPDX-License-Identifier: Apache-2.0
//
// The GPU v210 decode (#238) against the CPU convert it replaces. v210::DecodeToP016 runs
// shaders/v210_decode.inc, the compute shader's own source compiled as C++, over every
// invocation of the dispatch; its P016 must equal livepix::V210ToP016's (the DeckLink path
// today) bit for bit, for one eye and a side-by-side pair, at widths that are and are not a
// multiple of 6 and that do and do not fill the 48-pixel row padding. Plus the v210 frame's
// layout in the upload thread's staging, and the test signal's v210 packing.
//
// The decode is matrix-blind on purpose (it emits P016 samples; the frame's matrix and range
// go to the MP_GRADE shader as for any P016 frame), so it is checked under each matrix tag to
// show the tag does not change a sample, and the tag a v210 frame carries is the one the CPU
// convert would have put on its P016. Pure: no GPU.

#include "media/V210.h"
#include "pro/LivePixels.h"
#include "pro/TestSignalSource.h"
#include "rhi/AsyncUpload.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

using mp::ColorMatrix;
namespace v210 = mp::v210;
namespace livepix = mp::livepix;

void RowBytes() {
    CHECK(v210::RowBytes(1920) == 5120, "1920: 40 x 48 px, no padding");
    CHECK(v210::RowBytes(3840) == 10240, "3840");
    CHECK(v210::RowBytes(1280) == 3456, "1280: rounded up to 1296 px");
    CHECK(v210::RowBytes(6) == 128 && v210::RowBytes(48) == 128 && v210::RowBytes(50) == 256, "small widths");
    CHECK(v210::RowBytes(2048) == 5504, "2048 (DCI 2K): 43 x 128");
    CHECK(v210::RowBytes(0) == 0, "empty");
}

// One eye of random 10-bit 4:2:2 (deterministic), packed with PackRow.
struct Eye {
    int w = 0, h = 0;
    std::vector<uint16_t> y, cb, cr;  // y: w x h; cb / cr: w/2 x h
    std::vector<uint8_t> v210;
};
Eye MakeEye(int w, int h, uint32_t seed) {
    Eye e;
    e.w = w;
    e.h = h;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(0, 1023);  // the whole 10-bit range, out-of-legal included
    e.y.resize((size_t)w * h);
    e.cb.resize((size_t)(w / 2) * h);
    e.cr.resize(e.cb.size());
    for (auto& v : e.y) v = (uint16_t)d(rng);
    for (auto& v : e.cb) v = (uint16_t)d(rng);
    for (auto& v : e.cr) v = (uint16_t)d(rng);
    const size_t rb = v210::RowBytes(w);
    e.v210.assign(rb * h, 0xEE);  // PackRow must clear the padding itself
    for (int r = 0; r < h; ++r)
        v210::PackRow(
            e.v210.data() + rb * r, w, [&](int x) { return e.y[(size_t)r * w + x]; },
            [&](int i) { return e.cb[(size_t)r * (w / 2) + i]; }, [&](int i) { return e.cr[(size_t)r * (w / 2) + i]; });
    return e;
}

// GPU reference vs CPU convert for `eyes` eyes of w x h. Returns the largest sample difference.
int CompareDecode(int eyes, int w, int h, uint32_t seed) {
    Eye e[2] = {MakeEye(w, h, seed), MakeEye(w, h, seed * 7 + 1)};
    const int W = eyes * w;
    // The CPU convert, as the DeckLink path runs it: each eye into its column of the frame.
    livepix::P016 cpu;
    cpu.Resize(W, h);
    for (int k = 0; k < eyes; ++k)
        livepix::V210ToP016(e[k].v210.data(), (int)v210::RowBytes(w), w, h, cpu, k * w);
    // The shader's arithmetic.
    const v210::OutLayout L = v210::OutLayoutFor(eyes, w, h);
    std::vector<uint32_t> out(L.totalBytes / 4, 0xDEADBEEFu);
    const uint8_t* src[2] = {e[0].v210.data(), e[1].v210.data()};
    v210::DecodeToP016(src, eyes, w, h, out.data());
    const uint16_t* gy = reinterpret_cast<const uint16_t*>(out.data());
    const uint16_t* guv = reinterpret_cast<const uint16_t*>(out.data() + L.uvBase);
    int maxDiff = 0;
    for (size_t i = 0; i < cpu.y.size(); ++i) maxDiff = std::max(maxDiff, std::abs((int)cpu.y[i] - (int)gy[i]));
    for (size_t i = 0; i < cpu.uv.size(); ++i) maxDiff = std::max(maxDiff, std::abs((int)cpu.uv[i] - (int)guv[i]));
    // ... and both equal the packed samples (the CPU convert is the oracle, but check it too).
    bool exact = true;
    for (int k = 0; k < eyes; ++k)
        for (int r = 0; r < h; ++r)
            for (int x = 0; x < w; ++x) {
                exact &= gy[(size_t)r * W + k * w + x] == (uint16_t)(e[k].y[(size_t)r * w + x] << 6);
                if ((r & 1) == 0 && (x & 1) == 0) {
                    const int i = x / 2;
                    const int cb = (e[k].cb[(size_t)r * (w / 2) + i] + e[k].cb[(size_t)(r + 1) * (w / 2) + i] + 1) >> 1;
                    const int cr = (e[k].cr[(size_t)r * (w / 2) + i] + e[k].cr[(size_t)(r + 1) * (w / 2) + i] + 1) >> 1;
                    exact &= guv[(size_t)(r / 2) * W + k * w + x] == (uint16_t)(cb << 6);
                    exact &= guv[(size_t)(r / 2) * W + k * w + x + 1] == (uint16_t)(cr << 6);
                }
            }
    if (!exact) maxDiff = std::max(maxDiff, 1 << 16);
    // Nothing written past the planes (the gap before uvBase is untouched).
    for (size_t i = L.yWords; i < L.uvBase; ++i)
        if (out[i] != 0xDEADBEEFu) maxDiff = std::max(maxDiff, 1 << 17);
    return maxDiff;
}

void DecodeMatchesCpu() {
    // Multiples of 6 and not; widths that fill the 48-px padding exactly (1920, 3840) and that
    // do not (1280, 1366, 2048, 722, 50); a lone partial group (2, 4).
    const int widths[] = {2, 4, 6, 8, 12, 50, 96, 100, 722, 1280, 1366, 1920, 2048, 3840};
    const int heights[] = {2, 4, 6, 18};
    int worst = 0;
    int cases = 0;
    for (int w : widths)
        for (int h : heights)
            for (int eyes = 1; eyes <= 2; ++eyes) {
                if (w >= 1280 && h > 6) continue;  // keep the big widths quick
                const int d = CompareDecode(eyes, w, h, (uint32_t)(w * 131 + h * 7 + eyes));
                worst = std::max(worst, d);
                ++cases;
                if (d != 0) {
                    char m[96];
                    std::snprintf(m, sizeof(m), "decode == CPU convert: %d eye(s) %dx%d (max diff %d)", eyes, w, h, d);
                    CHECK(false, m);
                }
            }
    // A real frame size once: two UHD eyes, a few rows (the full 2160 rows add nothing).
    worst = std::max(worst, CompareDecode(2, 3840, 8, 99));
    std::printf("  decode vs CPU convert: %d cases + 2x3840x8, max |diff| %d code values (16-bit)\n", cases + 1,
                worst);
}

void MatrixTags() {
    // The decode never reads a tag: the same rows decode to the same samples under any matrix,
    // and the frame's own tag (the SDI rule the CPU convert uses) is what the renderer pushes.
    for (ColorMatrix m : {ColorMatrix::BT601, ColorMatrix::BT709, ColorMatrix::BT2020}) {
        CHECK(CompareDecode(2, 1920, 4, (uint32_t)m + 5) == 0, "samples independent of the matrix");
        (void)m;
    }
    CHECK(livepix::P016Matrix(livepix::RawFormat::V210, 1080, ColorMatrix::BT709) ==
              mp::DefaultMatrixForHeight(1080),
          "HD v210 is tagged BT.709 by the CPU convert, as the raw frame is");
    CHECK(livepix::P016Matrix(livepix::RawFormat::V210, 486, ColorMatrix::BT709) == ColorMatrix::BT601,
          "SD v210: BT.601");
}

void FrameShapeAndLayout() {
    mp::FrameRing::Frame f;
    f.format = mp::PixFormat::V210;
    f.eyes = 2;
    f.width = 2 * 1280;
    f.height = 720;
    const size_t eyeBytes = v210::RowBytes(1280) * 720;
    f.plane[0].resize(eyeBytes);
    f.plane[1].resize(eyeBytes);
    int eyes = 0, eyeW = 0;
    CHECK(v210::Shape(f, eyes, eyeW) && eyes == 2 && eyeW == 1280, "a 2-eye 1280x720 v210 frame");
    namespace au = mp::asyncup;
    au::Layout l;
    CHECK(au::LayoutFor(f, l, true) && l.format == 3 && l.planes == 2, "staged as two raw eye planes");
    CHECK(l.rowBytes[0] == v210::RowBytes(1280) && l.h[0] == 720 && l.w[0] == 1280, "eye plane: the card's rows");
    CHECK(l.offset[0] == 0 && l.offset[1] % au::kPlaneAlign == 0 && l.offset[1] >= eyeBytes, "eye 1 aligned after eye 0");
    CHECK(l.total >= l.offset[1] + eyeBytes && l.total % au::kPlaneAlign == 0, "slot size");
    CHECK(!au::LayoutFor(f, l, false), "not staged while the GPU decode is off (the CPU fallback reads the ring)");
    CHECK(au::UploadFormat(mp::PixFormat::V210) == 3, "upload format 3");
    // v210 sizes: smaller than the P016 it replaces.
    const size_t p016 = (size_t)2560 * 720 * 2 + (size_t)1280 * 360 * 4;
    CHECK(l.total < p016, "v210 is smaller than P016");

    f.plane[1].resize(eyeBytes - 1);
    CHECK(!v210::Shape(f, eyes, eyeW), "a short plane is refused");
    f.plane[1].resize(eyeBytes);
    f.width = 2 * 1281;
    CHECK(!v210::Shape(f, eyes, eyeW), "an odd eye width is refused");
    f.width = 2 * 1280;
    f.height = 721;
    CHECK(!v210::Shape(f, eyes, eyeW), "an odd height is refused");
    f.height = 720;
    f.eyes = 1;
    f.width = 1280;
    CHECK(v210::Shape(f, eyes, eyeW) && eyes == 1 && eyeW == 1280, "mono");
    f.format = mp::PixFormat::P016;
    CHECK(!v210::Shape(f, eyes, eyeW), "not v210");

    // 8K station numbers, for the PR: bytes per frame staged.
    const size_t v8k = 2 * v210::RowBytes(3840) * 2160, p8k = (size_t)7680 * 2160 * 2 + (size_t)3840 * 1080 * 4;
    std::printf("  bytes per 2x UHD frame: v210 %.1f MB, P016 %.1f MB (%.0f%%)\n", v8k / 1048576.0, p8k / 1048576.0,
                100.0 * v8k / p8k);
}

void Requested() {
    CHECK(!v210::Requested(nullptr) && !v210::Requested("") && !v210::Requested("0") && !v210::Requested("off"),
          "off by default");
    CHECK(v210::Requested("1") && v210::Requested("ON") && v210::Requested("true") && v210::Requested("yes"), "on");
}

void TestSignalPacking() {
    // The test signal's v210 arm: the raw rows decode to exactly the P016 its CPU arm publishes,
    // and the dithered low bits survive (10 bits, not 8 << 2).
    mp::TestSignalConfig cfg;
    cfg.eyeW = 1280;
    cfg.eyeH = 64;
    livepix::Nv12 frame;
    mp::TestSignalSource::Render(cfg, 3, frame);
    const int w = cfg.eyeW, h = cfg.eyeH;
    const size_t rb = v210::RowBytes(w);
    std::vector<uint8_t> eye[2];
    livepix::P016 cpu;
    cpu.Resize(2 * w, h);
    for (int e = 0; e < 2; ++e) {
        eye[e].resize(rb * h);
        mp::TestSignalSource::PackEyeV210(frame, e * w, w, h, eye[e].data());
        livepix::ToP016(livepix::RawFormat::V210, eye[e].data(), (int)rb, w, h, cpu, e * w, ColorMatrix::BT709);
    }
    const v210::OutLayout L = v210::OutLayoutFor(2, w, h);
    std::vector<uint32_t> out(L.totalBytes / 4);
    const uint8_t* src[2] = {eye[0].data(), eye[1].data()};
    v210::DecodeToP016(src, 2, w, h, out.data());
    CHECK(std::memcmp(out.data(), cpu.y.data(), cpu.y.size() * 2) == 0, "test signal: raw arm Y == CPU arm Y");
    CHECK(std::memcmp(out.data() + L.uvBase, cpu.uv.data(), cpu.uv.size() * 2) == 0,
          "test signal: raw arm CbCr == CPU arm CbCr");
    bool lowBits = false;
    for (uint16_t v : cpu.y) lowBits |= ((v >> 6) & 3) != 0;
    CHECK(lowBits, "test signal: 10-bit samples");
}

}  // namespace

int main() {
    std::printf("v210_test\n");
    RowBytes();
    DecodeMatchesCpu();
    MatrixTags();
    FrameShapeAndLayout();
    Requested();
    TestSignalPacking();
    if (g_failures) {
        std::fprintf(stderr, "v210_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("v210_test: OK\n");
    return 0;
}
