// SPDX-License-Identifier: BSL-1.0
//
// Synthetic round-trip test for LifLoader. We don't ship a real .lif fixture, so the
// test *builds* valid LIF containers in memory (a base JPEG + an embedded second-view
// JPEG + a JSON metadata field + the trailer) and drives them through
// LifLoader::Load, asserting the binary parse, left/right ordering, SBS compose, and
// the mono/non-LIF fallbacks. This exercises every byte of the container reader
// without hardware or a runtime.
//
// Container layout reference: dfattal/LIF-renderer src/LifLoader.ts (mirrored by
// src/media/LifLoader.cpp).

#include "media/LifLoader.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using mp::LifLoader;
using mp::LifResult;
using mp::StereoLayout;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// --- big-endian append helpers (the LIF trailer/field table is big-endian) ------
void PutU32BE(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v >> 24));
    b.push_back((uint8_t)(v >> 16));
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)v);
}
void PutU16BE(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)v);
}
void Append(std::vector<uint8_t>& b, const std::vector<uint8_t>& more) {
    b.insert(b.end(), more.begin(), more.end());
}

// --- a solid-color RGB JPEG in memory (quality high so the round-trip stays near the
//     source color; LifLoader/stb expand it to RGBA on decode) -----------------------
void JpegSink(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}
std::vector<uint8_t> MakeSolidJpeg(int w, int h, uint8_t r, uint8_t g, uint8_t bl) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        rgb[i * 3 + 0] = r;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = bl;
    }
    std::vector<uint8_t> jpg;
    const int ok = stbi_write_jpg_to_func(&JpegSink, &jpg, w, h, 3, rgb.data(), 95);
    if (!ok) std::fprintf(stderr, "  (stbi_write_jpg_to_func failed)\n");
    return jpg;
}

// A field in the appended metadata region.
struct TestField {
    uint32_t type;
    std::vector<uint8_t> data;
};

// Assemble a LIF: base JPEG, then a metadata region of `fields`, then the 6-byte
// trailer (regionOffset u32 + magic 0x1E1A). Mirrors the real on-disk layout.
std::vector<uint8_t> BuildLif(const std::vector<uint8_t>& baseJpeg,
                              const std::vector<TestField>& fields) {
    std::vector<uint8_t> lif = baseJpeg;
    const size_t regionStart = lif.size();
    PutU32BE(lif, (uint32_t)fields.size());
    for (const auto& f : fields) {
        PutU32BE(lif, f.type);
        PutU32BE(lif, (uint32_t)f.data.size());
        Append(lif, f.data);
    }
    // regionOffset spans from regionStart to the very end of the file (incl. trailer).
    const uint32_t regionOffset = (uint32_t)(lif.size() + 6 - regionStart);
    PutU32BE(lif, regionOffset);
    PutU16BE(lif, 0x1E1A);
    return lif;
}

std::vector<uint8_t> JsonBytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Write bytes to a temp file and load via LifLoader (it reads from a path).
LifResult LoadFromBytes(const std::vector<uint8_t>& bytes, const char* name) {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / name;
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    }
    LifResult r = LifLoader::Load(p.string());
    std::error_code ec;
    fs::remove(p, ec);
    return r;
}

// Sample one RGBA pixel.
struct Px { int r, g, b; };
Px PixelAt(const mp::DecodedImage& img, int x, int y) {
    const size_t i = ((size_t)y * img.width + x) * 4;
    return Px{img.pixels[i], img.pixels[i + 1], img.pixels[i + 2]};
}
bool Reddish(const Px& p) { return p.r > 150 && p.b < 100; }
bool Bluish(const Px& p) { return p.b > 150 && p.r < 100; }

constexpr int kW = 16, kH = 12;

// Stereo LIF: base = RED (left view, blob_id -1), embedded = BLUE (right view, blob 1),
// ordered by camera x. Expect a 2W×H SBS with red on the left half, blue on the right.
void TestStereoComposed() {
    std::printf("[stereo compose]\n");
    const auto red = MakeSolidJpeg(kW, kH, 220, 30, 30);
    const auto blue = MakeSolidJpeg(kW, kH, 30, 30, 220);
    const std::string json =
        R"({"views":[{"image":{"blob_id":-1},"position":[-0.5,0,0]},)"
        R"({"image":{"blob_id":1},"position":[0.5,0,0]}]})";
    const auto lif = BuildLif(red, {{8, JsonBytes(json)}, {1, blue}});
    const LifResult r = LoadFromBytes(lif, "mp_lif_stereo.lif");

    CHECK(r.ok, "stereo: ok");
    CHECK(r.stereo, "stereo: stereo flag");
    CHECK(r.layout == StereoLayout::SbsFull, "stereo: SbsFull layout");
    CHECK(r.image.width == kW * 2, "stereo: width == 2W");
    CHECK(r.image.height == kH, "stereo: height == H");
    if (r.image.Valid() && r.image.width == kW * 2) {
        CHECK(Reddish(PixelAt(r.image, kW / 2, kH / 2)), "stereo: left half is red");
        CHECK(Bluish(PixelAt(r.image, kW + kW / 2, kH / 2)), "stereo: right half is blue");
    }
}

// Same views but reversed in the array AND blob mapping, with positions still encoding
// L/R. Ordering must follow position.x, not array order: red still ends up on the left.
void TestOrderingByPosition() {
    std::printf("[ordering by position.x]\n");
    const auto blue = MakeSolidJpeg(kW, kH, 30, 30, 220);   // base = blue this time
    const auto red = MakeSolidJpeg(kW, kH, 220, 30, 30);    // embedded = red
    // views[0] = base(blue) at x=+0.5 (right); views[1] = embedded(red) at x=-0.5 (left).
    const std::string json =
        R"({"views":[{"image":{"blob_id":-1},"position":[0.5,0,0]},)"
        R"({"image":{"blob_id":1},"position":[-0.5,0,0]}]})";
    const auto lif = BuildLif(blue, {{8, JsonBytes(json)}, {1, red}});
    const LifResult r = LoadFromBytes(lif, "mp_lif_order.lif");

    CHECK(r.ok && r.stereo, "ordering: ok + stereo");
    if (r.image.Valid() && r.image.width == kW * 2) {
        CHECK(Reddish(PixelAt(r.image, kW / 2, kH / 2)), "ordering: red on left (by position)");
        CHECK(Bluish(PixelAt(r.image, kW + kW / 2, kH / 2)), "ordering: blue on right (by position)");
    }
}

// Classic Leia LIF schema (as shipped in real .jpg files): RGB keyed by top-level
// "albedoId" (-1 = base), position by "xLocation", JSON in the legacy type-7 field.
// Must compose just like the modern schema: base(red) at xLocation 0 → left.
void TestClassicLeiaSchema() {
    std::printf("[classic Leia schema: albedoId/xLocation, type-7 JSON]\n");
    const auto red = MakeSolidJpeg(kW, kH, 220, 30, 30);    // base = left
    const auto blue = MakeSolidJpeg(kW, kH, 30, 30, 220);   // embedded = right
    const std::string json =
        R"({"isCAI":false,"viewMode":"LIGHTFIELD","views":[)"
        R"({"albedoId":-1,"disparityId":0,"xLocation":0.0,"yLocation":0.0},)"
        R"({"albedoId":1000001,"disparityId":0,"xLocation":1.0,"yLocation":0.0}]})";
    // JSON in the legacy type-7 field; right view blob under type 1000001.
    const auto lif = BuildLif(red, {{7, JsonBytes(json)}, {1000001, blue}});
    const LifResult r = LoadFromBytes(lif, "mp_lif_classic.lif");

    CHECK(r.ok, "classic: ok");
    CHECK(r.stereo, "classic: stereo");
    CHECK(r.layout == StereoLayout::SbsFull, "classic: SbsFull layout");
    CHECK(r.image.width == kW * 2 && r.image.height == kH, "classic: 2W×H");
    if (r.image.Valid() && r.image.width == kW * 2) {
        CHECK(Reddish(PixelAt(r.image, kW / 2, kH / 2)), "classic: red on left");
        CHECK(Bluish(PixelAt(r.image, kW + kW / 2, kH / 2)), "classic: blue on right");
    }
}

// One-view LIF: not stereo → decode the base JPEG as flat 2D (Mono), W×H.
void TestMonoFallbackOneView() {
    std::printf("[mono fallback: single view]\n");
    const auto red = MakeSolidJpeg(kW, kH, 220, 30, 30);
    const std::string json = R"({"views":[{"image":{"blob_id":-1},"position":[0,0,0]}]})";
    const auto lif = BuildLif(red, {{8, JsonBytes(json)}});
    const LifResult r = LoadFromBytes(lif, "mp_lif_mono.lif");

    CHECK(r.ok, "mono1: ok");
    CHECK(!r.stereo, "mono1: not stereo");
    CHECK(r.layout == StereoLayout::Mono, "mono1: Mono layout");
    CHECK(r.image.width == kW && r.image.height == kH, "mono1: W×H (not doubled)");
}

// No LIF trailer at all (a plain JPEG saved as .lif) → flat 2D, never a hard failure.
void TestNonLifFallback() {
    std::printf("[fallback: plain JPEG, no trailer]\n");
    const auto red = MakeSolidJpeg(kW, kH, 220, 30, 30);
    const LifResult r = LoadFromBytes(red, "mp_plain.lif");

    CHECK(r.ok, "plain: ok (decoded base JPEG)");
    CHECK(!r.stereo, "plain: not stereo");
    CHECK(r.layout == StereoLayout::Mono, "plain: Mono layout");
    CHECK(r.image.width == kW && r.image.height == kH, "plain: W×H");
}

// Valid trailer but truncated field data → must not crash; fall back to flat 2D.
void TestTruncatedRegion() {
    std::printf("[fallback: corrupt/truncated region]\n");
    auto red = MakeSolidJpeg(kW, kH, 220, 30, 30);
    const std::string json =
        R"({"views":[{"image":{"blob_id":-1},"position":[-0.5,0,0]},)"
        R"({"image":{"blob_id":1},"position":[0.5,0,0]}]})";
    auto lif = BuildLif(red, {{8, JsonBytes(json)}, {1, MakeSolidJpeg(kW, kH, 30, 30, 220)}});
    // Chop the middle of the file: leaves a valid trailer pointing into now-missing data.
    lif.erase(lif.begin() + (long)red.size() + 8, lif.begin() + (long)red.size() + 40);
    const LifResult r = LoadFromBytes(lif, "mp_lif_trunc.lif");
    CHECK(r.ok, "trunc: ok (fell back to base JPEG, no crash)");
}

} // namespace

int main() {
    std::printf("LifLoader synthetic round-trip test\n");
    TestStereoComposed();
    TestOrderingByPosition();
    TestClassicLeiaSchema();
    TestMonoFallbackOneView();
    TestNonLifFallback();
    TestTruncatedRegion();
    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
