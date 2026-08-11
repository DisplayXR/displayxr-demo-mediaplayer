// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for MpoLoader (#45).
//
// No fixture files: the test BUILDS valid MPO containers in memory — two real JPEGs
// (written with stb_image_write) plus a hand-assembled APP2 "MPF\0" segment carrying a
// TIFF header and an MP Index IFD — and drives them through MpoLoader::Load. Assembling
// the container by hand is also a cross-check of the parser against the spec, since the
// two were written from the same layout description rather than from each other.
//
// The malformed cases matter as much as the happy path: MPO offsets are attacker-
// controlled, so every one of these must return ok=false without reading out of bounds.

#include "media/MpoLoader.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using mp::MpoLoader;
using mp::MpoResult;
using mp::StereoLayout;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// --- JPEG synthesis ----------------------------------------------------------------

void JpegSink(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

// A textured RGB JPEG. `phase` shifts the pattern horizontally, so two calls differing
// only in phase give a genuine stereo pair.
std::vector<uint8_t> MakeJpeg(int w, int h, int phase) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float fx = (float)(x + phase), fy = (float)y;
            float v = 128.0f + 50.0f * std::sin(fx * 0.05f + fy * 0.02f) +
                      35.0f * std::sin(fx * 0.17f - fy * 0.09f);
            const uint8_t c = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            uint8_t* q = &rgb[((size_t)y * w + x) * 3];
            q[0] = c; q[1] = (uint8_t)(255 - c); q[2] = c;
        }
    std::vector<uint8_t> out;
    stbi_write_jpg_to_func(JpegSink, &out, w, h, 3, rgb.data(), 92);
    return out;
}

// --- MPF container assembly ---------------------------------------------------------

struct EntrySpec {
    uint32_t attr = 0;
    uint32_t size = 0;
    uint32_t offset = 0;   // relative to the TIFF base; 0 for the primary
};

struct MpfOpts {
    bool little = true;
    uint32_t numImages = 2;
    int32_t ifdOffsetOverride = -1;      // -1 = the correct value (8)
    int32_t mpEntryOffsetOverride = -1;  // -1 = the correct value (50)
    uint16_t magicOverride = 42;
};

void Put16(std::vector<uint8_t>& b, uint16_t v, bool le) {
    if (le) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
    else    { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
}
void Put32(std::vector<uint8_t>& b, uint32_t v, bool le) {
    if (le) {
        b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8));
        b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 24));
    } else {
        b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16));
        b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v);
    }
}

// Everything after "MPF\0". Fixed shape:
//   0..7   TIFF header
//   8      MP Index IFD: u16 count=3, 3x12-byte entries, u32 next=0   (ends at 50)
//   50     MP Entries: 16 bytes each
constexpr uint32_t kIfdOff = 8;
constexpr uint32_t kMpEntryOff = 50;

std::vector<uint8_t> MpfPayload(const std::vector<EntrySpec>& entries, const MpfOpts& o) {
    const bool le = o.little;
    const uint32_t ifdOff = o.ifdOffsetOverride < 0 ? kIfdOff : (uint32_t)o.ifdOffsetOverride;
    const uint32_t mpOff =
        o.mpEntryOffsetOverride < 0 ? kMpEntryOff : (uint32_t)o.mpEntryOffsetOverride;

    std::vector<uint8_t> p;
    p.push_back(le ? 0x49 : 0x4D);
    p.push_back(le ? 0x49 : 0x4D);
    Put16(p, o.magicOverride, le);
    Put32(p, ifdOff, le);

    // IFD
    Put16(p, 3, le);                                   // entry count
    Put16(p, 0xB000, le); Put16(p, 7, le); Put32(p, 4, le);          // MPFVersion
    p.push_back('0'); p.push_back('1'); p.push_back('0'); p.push_back('0');
    Put16(p, 0xB001, le); Put16(p, 4, le); Put32(p, 1, le);          // NumberOfImages
    Put32(p, o.numImages, le);
    Put16(p, 0xB002, le); Put16(p, 7, le);                           // MPEntry
    Put32(p, (uint32_t)(entries.size() * 16), le);
    Put32(p, mpOff, le);
    Put32(p, 0, le);                                   // next IFD

    while (p.size() < kMpEntryOff) p.push_back(0);     // pad to the entry array
    for (const EntrySpec& e : entries) {
        Put32(p, e.attr, le);
        Put32(p, e.size, le);
        Put32(p, e.offset, le);
        Put16(p, 0, le);
        Put16(p, 0, le);
    }
    return p;
}

// The TIFF base sits at: SOI(2) + marker(2) + length(2) + "MPF\0"(4).
constexpr size_t kTiffBase = 10;

// Splice an APP2 MPF segment in right after SOI, then append the second image.
std::vector<uint8_t> BuildMpo(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                              const MpfOpts& o, bool secondIsThumb = false,
                              bool corruptSecondOffset = false) {
    // Two passes: the payload's size is fixed by its shape, so measure it with dummy
    // entries first, then fill in the real offsets.
    const size_t payloadSize = MpfPayload({EntrySpec{}, EntrySpec{}}, o).size();
    const size_t app2Total = 2 /*FFE2*/ + 2 /*len*/ + 4 /*MPF\0*/ + payloadSize;
    // primary = SOI(2) + APP2 + everything in `a` after ITS SOI(2)
    const size_t aPrimeSize = 2 + app2Total + (a.size() - 2);

    const uint32_t secondFileOff = (uint32_t)aPrimeSize;
    const uint32_t secondRel =
        corruptSecondOffset ? 0x7FFFFFFFu : (uint32_t)(secondFileOff - kTiffBase);

    std::vector<EntrySpec> entries = {
        EntrySpec{0x030000u, (uint32_t)aPrimeSize, 0u},                       // primary
        EntrySpec{secondIsThumb ? 0x010002u : 0x020002u, (uint32_t)b.size(), secondRel},
    };
    const std::vector<uint8_t> payload = MpfPayload(entries, o);

    std::vector<uint8_t> out;
    out.push_back(0xFF); out.push_back(0xD8);            // SOI
    out.push_back(0xFF); out.push_back(0xE2);            // APP2
    const uint16_t segLen = (uint16_t)(2 + 4 + payload.size());
    out.push_back((uint8_t)(segLen >> 8)); out.push_back((uint8_t)segLen);   // always BE
    out.push_back('M'); out.push_back('P'); out.push_back('F'); out.push_back(0);
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), a.begin() + 2, a.end());       // rest of the primary JPEG
    if (out.size() != aPrimeSize) {
        std::fprintf(stderr, "  NOTE: primary size %zu != predicted %zu\n", out.size(),
                     aPrimeSize);
    }
    out.insert(out.end(), b.begin(), b.end());           // the second image
    return out;
}

std::string WriteTemp(const std::string& name, const std::vector<uint8_t>& bytes) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    f.close();
    return p.string();
}

// --- tests ---------------------------------------------------------------------------

void TestValidMpo(bool little) {
    const int w = 320, h = 240;
    const std::vector<uint8_t> a = MakeJpeg(w, h, 0);
    const std::vector<uint8_t> b = MakeJpeg(w, h, 6);
    MpfOpts o;
    o.little = little;
    const std::string path =
        WriteTemp(little ? "dxr_mpo_le.mpo" : "dxr_mpo_be.mpo", BuildMpo(a, b, o));

    CHECK(MpoLoader::IsMpo(path), little ? "II: sniff detects MPF" : "MM: sniff detects MPF");
    const MpoResult r = MpoLoader::Load(path);
    CHECK(r.ok && r.stereo, little ? "II: two-view MPO loads" : "MM: two-view MPO loads");
    CHECK(r.layout == StereoLayout::SbsFull, "MPO composes to full SBS");
    CHECK(r.image.width == w * 2 && r.image.height == h, "MPO composed dims");
    std::filesystem::remove(path);
}

void TestPlainJpegRejected() {
    const std::vector<uint8_t> a = MakeJpeg(160, 120, 0);
    const std::string path = WriteTemp("dxr_plain.jpg", a);
    CHECK(!MpoLoader::IsMpo(path), "plain JPEG: sniff must say no");
    CHECK(!MpoLoader::Load(path).ok, "plain JPEG: Load must decline (caller falls back)");
    std::filesystem::remove(path);
}

void TestNotAJpeg() {
    const std::vector<uint8_t> junk(4096, 0xA5);
    const std::string path = WriteTemp("dxr_junk.bin", junk);
    CHECK(!MpoLoader::IsMpo(path), "non-JPEG: sniff must say no");
    CHECK(!MpoLoader::Load(path).ok, "non-JPEG: Load must decline");
    std::filesystem::remove(path);
}

// Every one of these is a malformed container: the requirement is ok=false and no
// out-of-bounds read (the sanitizer/ASAN build is what proves the second half).
void TestMalformed() {
    const std::vector<uint8_t> a = MakeJpeg(160, 120, 0);
    const std::vector<uint8_t> b = MakeJpeg(160, 120, 5);

    {   // IFD offset points past the end of the buffer
        MpfOpts o;
        o.ifdOffsetOverride = 0x7FFFFFF0;
        const std::string p = WriteTemp("dxr_mpo_badifd.mpo", BuildMpo(a, b, o));
        CHECK(!MpoLoader::Load(p).ok, "malformed: out-of-range IFD offset");
        std::filesystem::remove(p);
    }
    {   // MP entry array points past the end
        MpfOpts o;
        o.mpEntryOffsetOverride = 0x7FFFFFF0;
        const std::string p = WriteTemp("dxr_mpo_badentries.mpo", BuildMpo(a, b, o));
        CHECK(!MpoLoader::Load(p).ok, "malformed: out-of-range MP entry offset");
        std::filesystem::remove(p);
    }
    {   // second image's data offset points past the end
        MpfOpts o;
        const std::string p = WriteTemp("dxr_mpo_badimg.mpo", BuildMpo(a, b, o, false, true));
        CHECK(!MpoLoader::Load(p).ok, "malformed: out-of-range image offset");
        std::filesystem::remove(p);
    }
    {   // wrong TIFF magic
        MpfOpts o;
        o.magicOverride = 1234;
        const std::string p = WriteTemp("dxr_mpo_badmagic.mpo", BuildMpo(a, b, o));
        CHECK(!MpoLoader::Load(p).ok, "malformed: bad TIFF magic");
        std::filesystem::remove(p);
    }
    {   // says it holds only one image
        MpfOpts o;
        o.numImages = 1;
        const std::string p = WriteTemp("dxr_mpo_one.mpo", BuildMpo(a, b, o));
        CHECK(!MpoLoader::Load(p).ok, "malformed: NumberOfImages < 2");
        std::filesystem::remove(p);
    }
    {   // the only other image is a large thumbnail, not a view
        MpfOpts o;
        const std::string p = WriteTemp("dxr_mpo_thumb.mpo", BuildMpo(a, b, o, true));
        CHECK(!MpoLoader::Load(p).ok, "thumbnail-class second entry is not a view");
        std::filesystem::remove(p);
    }
    {   // the two images disagree on size — the reliable filter for a mis-tagged preview
        const std::vector<uint8_t> small = MakeJpeg(80, 60, 5);
        MpfOpts o;
        const std::string p = WriteTemp("dxr_mpo_mismatch.mpo", BuildMpo(a, small, o));
        CHECK(!MpoLoader::Load(p).ok, "view dimension mismatch is not a stereo pair");
        std::filesystem::remove(p);
    }
}

} // namespace

int main() {
    std::printf("mpo_parse_test\n");
    TestValidMpo(true);
    TestValidMpo(false);
    TestPlainJpegRejected();
    TestNotAJpeg();
    TestMalformed();

    if (g_failures == 0) {
        std::printf("  all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "  %d check(s) failed\n", g_failures);
    return 1;
}
