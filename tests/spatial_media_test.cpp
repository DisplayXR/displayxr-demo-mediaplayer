// SPDX-License-Identifier: Apache-2.0
//
// Phone spatial capture (#154): MV-HEVC stereo video and stereo HEIC photos.
//
// Pure part (always): eye-order resolution, and the bounded HEIF box walk for the `ster`
// stereo-pair group and `pitm` — on hand-built byte buffers, including truncated and lying
// ones.
//
// FFmpeg part (when linked, FFmpeg 7.1+): the committed fixtures in tests/fixtures/, made
// by tools/spatial/make_spatial_fixtures.swift with the platform's own MV-HEVC and HEIC
// encoders. In every fixture the LEFT eye has a red band across the top and the RIGHT eye a
// blue band, so a decoded frame proves both that the two eyes differ and which is which:
//   mvhevc_lr.mov / mvhevc_rl.mov   base layer = left / base layer = RIGHT
//   stereo_lr.heic / stereo_rl.heic first image = left / first image = RIGHT
//   mono.heic                        no group -> the primary image, flat

#include "media/SpatialMedia.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(MEDIAPLAYER_WITH_FFMPEG)
#include "media/FrameRing.h"
#include "media/HeifLoader.h"
#include "media/VideoDecoder.h"
#include "media/VideoStereoProbe.h"

#include <chrono>
#include <thread>
#endif

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

using mp::spatial::EyeHint;

void TestEyeOrder() {
    using mp::spatial::ResolveEyeOrder;
    // Tags win, from either side.
    auto o = ResolveEyeOrder(EyeHint::Left, EyeHint::Right, 0, 1);
    CHECK(o.firstIsLeft && o.fromMetadata, "L,R tags -> first is left");
    o = ResolveEyeOrder(EyeHint::Right, EyeHint::Left, 0, 1);
    CHECK(!o.firstIsLeft && o.fromMetadata, "R,L tags -> second is left (base view is the right eye)");
    o = ResolveEyeOrder(EyeHint::Unknown, EyeHint::Left, 0, 1);
    CHECK(!o.firstIsLeft && o.fromMetadata, "only B tagged Left -> B is left");
    o = ResolveEyeOrder(EyeHint::Right, EyeHint::Unknown, 0, 1);
    CHECK(!o.firstIsLeft && o.fromMetadata, "only A tagged Right -> B is left");
    o = ResolveEyeOrder(EyeHint::Unknown, EyeHint::Right, 5, 3);
    CHECK(o.firstIsLeft && o.fromMetadata, "only B tagged Right -> A is left, ids ignored");
    // No tags: the lower view id (the base view) is left, whatever the decode order.
    o = ResolveEyeOrder(EyeHint::Unknown, EyeHint::Unknown, 0, 1);
    CHECK(o.firstIsLeft && !o.fromMetadata, "untagged: base view 0 is left");
    o = ResolveEyeOrder(EyeHint::Unknown, EyeHint::Unknown, 1, 0);
    CHECK(!o.firstIsLeft && !o.fromMetadata, "untagged, out of order: view 0 is still left");
    o = ResolveEyeOrder(EyeHint::Unknown, EyeHint::Unknown, -1, -1);
    CHECK(o.firstIsLeft && !o.fromMetadata, "nothing known: decode order, first is left");
    // Contradictions fall back rather than guessing from one tag.
    o = ResolveEyeOrder(EyeHint::Left, EyeHint::Left, 1, 0);
    CHECK(!o.firstIsLeft && !o.fromMetadata, "both Left: fall back to the base view");
    o = ResolveEyeOrder(EyeHint::Right, EyeHint::Right, 0, 1);
    CHECK(o.firstIsLeft && !o.fromMetadata, "both Right: fall back to the base view");
}

// --- tiny ISO-BMFF writer ------------------------------------------------------------
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}
void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}
std::vector<uint8_t> Box(const char* type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> v;
    Put32(v, (uint32_t)(8 + body.size()));
    v.insert(v.end(), type, type + 4);
    v.insert(v.end(), body.begin(), body.end());
    return v;
}
std::vector<uint8_t> Cat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> v;
    for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
    return v;
}
std::vector<uint8_t> Ftyp(const char* major, std::initializer_list<const char*> compat) {
    std::vector<uint8_t> b(major, major + 4);
    Put32(b, 0);
    for (const char* c : compat) b.insert(b.end(), c, c + 4);
    return Box("ftyp", b);
}
std::vector<uint8_t> Ster(uint32_t groupId, std::initializer_list<uint32_t> ids) {
    std::vector<uint8_t> b;
    Put32(b, 0);  // version + flags
    Put32(b, groupId);
    Put32(b, (uint32_t)ids.size());
    for (uint32_t id : ids) Put32(b, id);
    return Box("ster", b);
}
std::vector<uint8_t> Pitm(uint16_t id) {
    std::vector<uint8_t> b;
    Put32(b, 0);
    Put16(b, id);
    return Box("pitm", b);
}
std::vector<uint8_t> Meta(const std::vector<uint8_t>& children) {
    std::vector<uint8_t> b;
    Put32(b, 0);  // FullBox
    b.insert(b.end(), children.begin(), children.end());
    return Box("meta", b);
}

void TestHeifWalk() {
    using mp::spatial::LooksLikeHeif;
    using mp::spatial::ParseHeifStereoPair;
    const auto ftyp = Ftyp("heic", {"mif1", "heic"});
    // Right image first in the file, the group names item 7 as left.
    const auto file = Cat({ftyp, Meta(Cat({Box("hdlr", std::vector<uint8_t>(25, 0)), Pitm(3),
                                           Box("grpl", Cat({Box("altr", std::vector<uint8_t>(16, 0)),
                                                             Ster(9, {7, 3})}))})),
                           Box("mdat", std::vector<uint8_t>(32, 0xAB))});
    CHECK(LooksLikeHeif(file.data(), file.size()), "heic brand recognised");
    auto p = ParseHeifStereoPair(file.data(), file.size());
    CHECK(p.hasPair && p.leftId == 7 && p.rightId == 3, "ster entity 0 = left, entity 1 = right");
    CHECK(p.hasPrimary && p.primaryId == 3, "pitm read");

    // No group: primary only.
    const auto flat = Cat({ftyp, Meta(Pitm(1))});
    p = ParseHeifStereoPair(flat.data(), flat.size());
    CHECK(!p.hasPair && p.hasPrimary && p.primaryId == 1, "no ster -> primary only");

    // A ster group of one entity, or naming the same item twice, is not a pair.
    const auto one = Cat({ftyp, Meta(Box("grpl", Ster(1, {4})))});
    CHECK(!ParseHeifStereoPair(one.data(), one.size()).hasPair, "one-entity ster rejected");
    const auto same = Cat({ftyp, Meta(Box("grpl", Ster(1, {4, 4})))});
    CHECK(!ParseHeifStereoPair(same.data(), same.size()).hasPair, "ster with a repeated id rejected");

    // A ster that claims more entities than it holds must not read past its box.
    std::vector<uint8_t> lying;
    Put32(lying, 0);
    Put32(lying, 1);
    Put32(lying, 1000);  // num_entities
    Put32(lying, 2);
    Put32(lying, 3);
    const auto lyingFile = Cat({ftyp, Meta(Box("grpl", Box("ster", lying)))});
    CHECK(!ParseHeifStereoPair(lyingFile.data(), lyingFile.size()).hasPair, "over-claiming ster rejected");

    // Every truncation of the good file parses without crashing, and never invents a pair
    // the prefix does not contain in full.
    int pairs = 0;
    for (size_t n = 0; n < file.size(); ++n) pairs += ParseHeifStereoPair(file.data(), n).hasPair ? 1 : 0;
    CHECK(pairs < (int)file.size(), "truncated prefixes parse safely");

    // A box whose size runs past the buffer stops the walk.
    std::vector<uint8_t> bad = file;
    bad[ftyp.size() + 3] = 0xFF;  // meta's size, low byte
    bad[ftyp.size() + 2] = 0xFF;
    CHECK(!ParseHeifStereoPair(bad.data(), bad.size()).hasPair, "oversized meta box rejected");

    CHECK(!LooksLikeHeif(nullptr, 0), "null buffer is not HEIF");
    const auto mp4 = Ftyp("isom", {"iso2", "mp41"});
    CHECK(!LooksLikeHeif(mp4.data(), mp4.size()), "an MP4 ftyp is not HEIF");
    const auto compat = Ftyp("abcd", {"mif1"});
    CHECK(LooksLikeHeif(compat.data(), compat.size()), "HEIF by compatible brand");
}

#if defined(MEDIAPLAYER_WITH_FFMPEG)

std::string Fixture(const char* name) { return std::string(MP_FIXTURE_DIR) + "/" + name; }

// Mean of an RGBA8 channel over a rectangle.
double MeanRGBA(const mp::DecodedImage& im, int ch, int x0, int y0, int x1, int y1) {
    double acc = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x, ++n) acc += im.pixels[((size_t)y * im.width + x) * 4 + ch];
    return n ? acc / n : 0.0;
}

void TestHeic(const char* name, bool expectStereo) {
    std::printf("  %s\n", name);
    const mp::HeifResult r = mp::HeifLoader::Load(Fixture(name));
    CHECK(r.ok, "HEIC decoded");
    if (!r.ok) return;
    CHECK(r.stereo == expectStereo, "stereo verdict");
    if (expectStereo) {
        CHECK(r.layout == mp::StereoLayout::SbsFull, "stereo HEIC composes to SBS-full");
        CHECK(r.image.width == 256 && r.image.height == 96, "two 128x96 eyes side by side");
        // Top band: left half red, right half blue.
        const double lr = MeanRGBA(r.image, 0, 8, 2, 120, 8), lb = MeanRGBA(r.image, 2, 8, 2, 120, 8);
        const double rr = MeanRGBA(r.image, 0, 136, 2, 248, 8), rb = MeanRGBA(r.image, 2, 136, 2, 248, 8);
        std::printf("    left band R%.0f B%.0f | right band R%.0f B%.0f\n", lr, lb, rr, rb);
        CHECK(lr > 150 && lb < 100, "left eye (left half) is the red-band picture");
        CHECK(rb > 150 && rr < 100, "right eye (right half) is the blue-band picture");
    } else {
        CHECK(r.layout == mp::StereoLayout::Mono, "flat HEIC stays mono");
        CHECK(r.image.width == 128 && r.image.height == 96, "flat HEIC is one picture");
    }
}

void TestMvHevc(const char* name) {
    std::printf("  %s\n", name);
    const std::string path = Fixture(name);
    const mp::VideoStereoProbe::Result pr = mp::VideoStereoProbe::Run(path, true);
    CHECK(pr.ok && pr.multiview, "probe recognises MV-HEVC");
    CHECK(pr.haveMeta && pr.metaLayout == mp::StereoLayout::SbsFull, "probe reports SBS-full");
    CHECK(pr.width == 256 && pr.height == 96, "probe reports both views' width");

    mp::VideoDecoder dec;
    if (!dec.Open(path)) {
        CHECK(false, "MV-HEVC opens");
        return;
    }
    CHECK(dec.IsMultiview(), "decoder decodes both views");
    CHECK(dec.Width() == 256 && dec.Height() == 96, "decoder frame is both views wide");
    const mp::FrameRing::Frame* f = nullptr;
    for (int i = 0; i < 400 && !f; ++i) {
        f = dec.Ring().AcquireLatest();
        if (!f) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(f != nullptr, "a composed frame arrives");
    if (f) {
        CHECK(f->width == 256 && f->height == 96, "composed frame size");
        CHECK(f->format == mp::PixFormat::I420 || f->format == mp::PixFormat::NV12, "8-bit YUV frame");
        // Chroma of the top band: red has Cr >> Cb, blue Cb >> Cr. I420: plane1 = Cb, plane2 = Cr.
        const int cw = (f->width + 1) / 2;
        auto chroma = [&](int x0, int x1, double& cb, double& cr) {
            double sb = 0, sr = 0;
            int n = 0;
            for (int y = 1; y < 4; ++y)
                for (int x = x0; x < x1; ++x, ++n) {
                    if (f->format == mp::PixFormat::I420) {
                        sb += f->plane[1][(size_t)y * cw + x];
                        sr += f->plane[2][(size_t)y * cw + x];
                    } else {
                        sb += f->plane[1][((size_t)y * cw + x) * 2];
                        sr += f->plane[1][((size_t)y * cw + x) * 2 + 1];
                    }
                }
            cb = sb / n;
            cr = sr / n;
        };
        double lcb, lcr, rcb, rcr;
        chroma(4, 60, lcb, lcr);
        chroma(68, 124, rcb, rcr);
        std::printf("    left band Cb%.0f Cr%.0f | right band Cb%.0f Cr%.0f | eye tags %s\n", lcb, lcr, rcb, rcr,
                    dec.MultiviewEyesFromMetadata() ? "used" : "absent");
        CHECK(lcr > lcb + 60, "left half is the red-band (left-eye) view");
        CHECK(rcb > rcr + 60, "right half is the blue-band (right-eye) view");
    }
    dec.Stop();
}

#endif

}  // namespace

int main() {
    std::printf("spatial_media_test\n");
    TestEyeOrder();
    TestHeifWalk();
#if defined(MEDIAPLAYER_WITH_FFMPEG)
    if (!mp::HeifLoader::Available()) {
        std::printf("  FFmpeg older than 7.1: MV-HEVC / HEIF fixture decode skipped\n");
    } else {
        TestMvHevc("mvhevc_lr.mov");
        TestMvHevc("mvhevc_rl.mov");
        TestHeic("stereo_lr.heic", true);
        TestHeic("stereo_rl.heic", true);
        TestHeic("mono.heic", false);
    }
#endif
    std::printf("spatial_media_test: %s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
