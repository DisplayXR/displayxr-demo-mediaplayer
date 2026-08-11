// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the stereo layout detector and the layered resolver (#45).
//
// No fixtures and no I/O: every buffer is synthesised in memory from a deterministic
// hash-PRNG, so the test is reproducible on every platform and needs neither media files
// nor a runtime. That is only possible because StereoDetect deliberately depends on
// nothing but StereoTypes.h — no SDL, no Vulkan, no libav.
//
// The cases that matter most are the ones the OLD aspect-ratio heuristic got wrong: a
// 16:9 half-SBS frame (dimensionally identical to mono), and a 2:1 mono photo (which any
// aspect threshold low enough to catch half-SBS will split down the middle).

#include "media/MediaSource.h"
#include "media/StereoDetect.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using mp::MediaInfo;
using mp::MediaKind;
using mp::MediaSource;
using mp::StereoDetect;
using mp::StereoDetectResult;
using mp::StereoLayout;
using mp::StereoSignal;
using mp::StereoVote;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// --- synthetic imagery ------------------------------------------------------------

// Cheap deterministic hash noise — same values on every platform, unlike rand().
float Hash(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

// Smooth value noise: hash lattice + smoothstep interpolation.
float ValueNoise(float x, float y, float scale) {
    const float fx = x / scale, fy = y / scale;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float tx = fx - (float)x0, ty = fy - (float)y0;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const float a = Hash(x0, y0), b = Hash(x0 + 1, y0);
    const float c = Hash(x0, y0 + 1), d = Hash(x0 + 1, y0 + 1);
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

// A textured plane with real detail at several scales.
//
// Two properties are load-bearing, and getting either wrong makes the test lie about the
// detector rather than about the code:
//
//   * APERIODIC. Sinusoids alias into a repeating pattern once the frame is box-averaged
//     down to a ~96-cell grid, and a repeating pattern correlates at many shifts -- which
//     the detector is specifically built to reject. A sinusoidal fixture therefore fails
//     every SBS case for the right reason and tells us nothing.
//   * SMOOTH. Blocky noise does not TRANSLATE: shifting by d pixels swaps whole lattice
//     cells for unrelated values instead of sliding the field, so a synthetic "disparity"
//     would decorrelate the pair rather than displace it.
float Texture(int x, int y) {
    const float fx = (float)x, fy = (float)y;
    float v = 20.0f;
    v += 120.0f * ValueNoise(fx, fy, 37.0f);
    v += 70.0f * ValueNoise(fx, fy, 13.0f);
    v += 40.0f * ValueNoise(fx, fy, 5.0f);
    return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
}

struct Img {
    std::vector<uint8_t> px;   // 8-bit luma, tightly packed
    int w = 0, h = 0;
    uint8_t& At(int x, int y) { return px[(size_t)y * w + x]; }
};

Img MakeImg(int w, int h) {
    Img im;
    im.w = w;
    im.h = h;
    im.px.assign((size_t)w * h, 0);
    return im;
}

// A full frame whose two halves are the same scene sampled `disparity` pixels apart.
// `eyeW`/`eyeH` are the per-half dims; `squeeze` halves the horizontal sampling rate so
// each eye is anamorphically compressed, i.e. half-SBS.
Img MakeSbs(int eyeW, int eyeH, int disparity, bool squeeze) {
    Img im = MakeImg(eyeW * 2, eyeH);
    const float xs = squeeze ? 2.0f : 1.0f;
    for (int y = 0; y < eyeH; ++y)
        for (int x = 0; x < eyeW; ++x) {
            const int sx = (int)((float)x * xs);
            im.At(x, y) = (uint8_t)Texture(sx, y);
            im.At(eyeW + x, y) = (uint8_t)Texture(sx + disparity, y);
        }
    return im;
}

// A mono frame: one continuous scene across the full width, so the two halves are
// genuinely different content.
Img MakeMono(int w, int h) {
    Img im = MakeImg(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) im.At(x, y) = (uint8_t)Texture(x, y);
    return im;
}

void FillRows(Img& im, int y0, int y1, uint8_t v) {
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < im.w; ++x) im.At(x, y) = v;
}
void FillCols(Img& im, int x0, int x1, uint8_t v) {
    for (int y = 0; y < im.h; ++y)
        for (int x = x0; x < x1; ++x) im.At(x, y) = v;
}

StereoDetectResult Run(Img& im) {
    return StereoDetect::AnalyzeLuma(im.px.data(), im.w, im.h, im.w, 1);
}

const char* LName(StereoLayout l) { return mp::LayoutName(l); }

// --- tests -------------------------------------------------------------------------

void TestSbsFull() {
    Img im = MakeSbs(1920, 1080, 8, false);
    const StereoDetectResult r = Run(im);
    CHECK(r.decided && !r.abstained, "full SBS: should decide");
    CHECK(r.layout == StereoLayout::SbsFull, "full SBS: layout");
    CHECK(r.peak > 0.9f, "full SBS: peak should be high");
    CHECK(r.margin > 0.1f, "full SBS: peak should be sharp");
    CHECK(std::abs(r.disparityPx) <= 6, "full SBS: disparity should be small");
    if (r.layout != StereoLayout::SbsFull)
        std::fprintf(stderr, "    got %s peak=%.3f margin=%.3f d=%d (%s)\n", LName(r.layout),
                     (double)r.peak, (double)r.margin, r.disparityPx, r.reason);
}

void TestSbsHalf() {
    // 1920x1080 total, each eye squeezed into 960 — pixel-for-pixel indistinguishable
    // from a mono 16:9 frame, which is the whole reason the content layer exists.
    Img im = MakeSbs(960, 1080, 6, true);
    const StereoDetectResult r = Run(im);
    CHECK(r.decided && r.layout == StereoLayout::SbsHalf, "half SBS: layout");
    if (r.layout != StereoLayout::SbsHalf)
        std::fprintf(stderr, "    got %s peak=%.3f margin=%.3f (%s)\n", LName(r.layout),
                     (double)r.peak, (double)r.margin, r.reason);
}

void TestMono16x9() {
    Img im = MakeMono(1920, 1080);
    const StereoDetectResult r = Run(im);
    CHECK(r.decided && r.layout == StereoLayout::Mono, "mono 16:9: should be confident mono");
}

void TestMono2x1() {
    // THE regression case: aspect 2.0 trips the old >=1.9 rule, but the halves are
    // unrelated, so a confident mono verdict must win.
    Img im = MakeMono(2160, 1080);
    const StereoDetectResult r = Run(im);
    CHECK(r.decided && r.layout == StereoLayout::Mono, "mono 2:1: content must beat aspect");

    MediaInfo info = MediaSource::Resolve("panorama.png", MediaKind::Image, 2160, 1080,
                                          nullptr, nullptr, &r);
    CHECK(info.layout == StereoLayout::Mono, "mono 2:1: Resolve must not fall through to aspect");
    CHECK(info.signal == StereoSignal::Content, "mono 2:1: signal should be Content");
}

void TestMono21x9() {
    Img im = MakeMono(2560, 1080);
    const StereoDetectResult r = Run(im);
    CHECK(r.decided && r.layout == StereoLayout::Mono, "mono 21:9 panorama: confident mono");
}

void TestLetterboxedSbs() {
    Img im = MakeSbs(1920, 1080, 8, false);
    FillRows(im, 0, 135, 0);
    FillRows(im, 945, 1080, 0);
    const StereoDetectResult r = Run(im);
    CHECK(r.decided && r.layout == StereoLayout::SbsFull, "letterboxed SBS: detect through bars");
    CHECK(r.cropY0 >= 130 && r.cropY1 <= 950, "letterboxed SBS: crop should find the bars");
}

void TestPillarboxedMono() {
    // Matching black bars correlate perfectly; without the crop this reads as stereo.
    Img im = MakeMono(2160, 1080);
    FillCols(im, 0, 270, 0);
    FillCols(im, 1890, 2160, 0);
    const StereoDetectResult r = Run(im);
    CHECK(r.layout == StereoLayout::Mono, "pillarboxed mono: bars must not fake a match");
    if (r.layout != StereoLayout::Mono)
        std::fprintf(stderr, "    got %s peak=%.3f margin=%.3f (%s)\n", LName(r.layout),
                     (double)r.peak, (double)r.margin, r.reason);
}

void TestPillarboxedSbs() {
    // Bars at 0, W/2 +/- a, W — the case a whole-frame crop pass cannot fix, because the
    // inner pair sits exactly at the correlation's centre.
    Img im = MakeSbs(1920, 1080, 8, false);
    FillCols(im, 0, 160, 0);
    FillCols(im, 1760, 2080, 0);
    FillCols(im, 3680, 3840, 0);
    const StereoDetectResult r = Run(im);
    CHECK(r.decided && r.layout == StereoLayout::SbsFull, "pillarboxed SBS: per-half crop");
    if (r.layout != StereoLayout::SbsFull)
        std::fprintf(stderr, "    got %s peak=%.3f margin=%.3f crop=[%d,%d) (%s)\n",
                     LName(r.layout), (double)r.peak, (double)r.margin, r.cropX0, r.cropX1,
                     r.reason);
}

void TestFlatAbstains() {
    Img im = MakeImg(3840, 1080);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) im.At(x, y) = (uint8_t)(48 + ((x + y) & 1));
    const StereoDetectResult r = Run(im);
    CHECK(r.abstained, "flat frame: must abstain, not guess");
}

void TestFadeBlackAbstains() {
    // An SBS pair scaled almost to black — exactly what frame 0 of a fade-in looks like.
    Img im = MakeSbs(1920, 1080, 8, false);
    for (auto& p : im.px) p = (uint8_t)(p / 24);
    const StereoDetectResult r = Run(im);
    CHECK(r.abstained, "fade-from-black frame: must abstain");
}

void TestPeriodicRejected() {
    // A brick-wall / railing / window-bay case: strong repeating structure shared by both
    // halves, but UNRELATED content behind it. The repeat makes the halves correlate at
    // many shifts, so the far baseline rises with the peak and the margin collapses --
    // which is exactly what the margin rule exists to catch. (Note the honest limit here:
    // two byte-identical periodic halves are information-theoretically indistinguishable
    // from a zero-disparity stereo pair, so that is not what is being asserted.)
    Img im = MakeImg(3840, 1080);
    const int mid = im.w / 2;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float bars = ((x % 160) < 80) ? 190.0f : 60.0f;
            const bool right = x >= mid;
            // Different noise per half => the halves are not the same scene.
            const float n = 34.0f * ValueNoise((float)(x + (right ? 9000 : 0)), (float)y, 21.0f);
            im.At(x, y) = (uint8_t)std::min(255.0f, bars + n);
        }
    const StereoDetectResult r = Run(im);
    CHECK(r.layout != StereoLayout::SbsFull && r.layout != StereoLayout::SbsHalf,
          "periodic content: repeating structure must not read as stereo");
    if (r.layout != StereoLayout::Mono)
        std::fprintf(stderr, "    got %s peak=%.3f margin=%.3f (%s)\n", LName(r.layout),
                     (double)r.peak, (double)r.margin, r.reason);
}

void TestGrossDisparityRejected() {
    Img im = MakeSbs(1920, 1080, 340, false);   // ~18% of half-width: not a stereo pair
    const StereoDetectResult r = Run(im);
    CHECK(!(r.decided && r.layout == StereoLayout::SbsFull),
          "gross offset: must not be accepted as SBS");
}

void TestRgbaMatchesLuma() {
    Img im = MakeSbs(1280, 720, 6, false);
    std::vector<uint8_t> rgba((size_t)im.w * im.h * 4);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const uint8_t v = im.At(x, y);
            uint8_t* q = &rgba[((size_t)y * im.w + x) * 4];
            q[0] = q[1] = q[2] = v;   // grey: Rec.601 weights sum to 1, so luma == v
            q[3] = 255;
        }
    const StereoDetectResult a = Run(im);
    const StereoDetectResult b =
        StereoDetect::AnalyzeRGBA(rgba.data(), im.w, im.h, (ptrdiff_t)im.w * 4);
    CHECK(a.layout == b.layout, "RGBA path: same layout as luma path");
    CHECK(std::fabs(a.peak - b.peak) < 0.02f, "RGBA path: same peak as luma path");
}

void TestRowStride() {
    Img im = MakeSbs(1280, 720, 6, false);
    const int pad = 37;
    std::vector<uint8_t> strided((size_t)(im.w + pad) * im.h, 0xAB);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) strided[(size_t)y * (im.w + pad) + x] = im.At(x, y);
    const StereoDetectResult a = Run(im);
    const StereoDetectResult b =
        StereoDetect::AnalyzeLuma(strided.data(), im.w, im.h, im.w + pad, 1);
    CHECK(a.layout == b.layout, "row stride: padding must not change the verdict");
    CHECK(std::fabs(a.peak - b.peak) < 1e-4f, "row stride: identical peak");
}

void TestPixelStride10Bit() {
    // Simulate a 10-bit little-endian plane: 2 bytes per sample, value in the high byte.
    Img im = MakeSbs(1280, 720, 6, false);
    std::vector<uint8_t> p16((size_t)im.w * im.h * 2, 0);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            uint8_t* q = &p16[((size_t)y * im.w + x) * 2];
            q[0] = 0x3F;             // low byte: noise the 8-bit path would read
            q[1] = im.At(x, y);      // high byte: the real value
        }
    const StereoDetectResult a = Run(im);
    const StereoDetectResult b =
        StereoDetect::AnalyzeLuma(p16.data() + 1, im.w, im.h, (ptrdiff_t)im.w * 2, 2);
    CHECK(a.layout == b.layout, "10-bit: high-byte sampling matches the 8-bit result");
}

void TestOddWidth() {
    Img im = MakeSbs(960, 540, 5, false);
    Img odd = MakeImg(im.w + 1, im.h);      // 1921-wide: centre column dropped
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) odd.At(x + (x >= im.w / 2 ? 1 : 0), y) = im.At(x, y);
    const StereoDetectResult r = Run(odd);
    CHECK(true, "odd width: must not crash");
    (void)r;
}

void TestVote() {
    StereoVote v;
    StereoDetectResult ab;
    ab.abstained = true;
    StereoDetectResult full;
    full.decided = true;
    full.layout = StereoLayout::SbsFull;
    full.confidence = 0.8f;
    StereoDetectResult mono;
    mono.decided = true;
    mono.layout = StereoLayout::Mono;
    mono.confidence = 0.5f;

    v.Add(ab); v.Add(ab); v.Add(full); v.Add(full); v.Add(mono); v.Add(full);
    CHECK(v.Samples() == 4, "vote: abstains must not count as samples");
    const StereoDetectResult r = v.Result();
    CHECK(r.decided, "vote: 4 samples clears the threshold");
    CHECK(r.layout == StereoLayout::SbsFull, "vote: majority wins");

    StereoVote few;
    few.Add(full);
    CHECK(!few.Result().decided, "vote: one sample is not enough on its own");
    few.AcceptSingle();
    CHECK(few.Result().decided, "vote: AcceptSingle rescues a very short stream");
}

void TestChooseFullOrHalf() {
    bool amb = false;
    CHECK(StereoDetect::ChooseFullOrHalf(3840.0f / 1080.0f, amb) == StereoLayout::SbsFull,
          "full/half: 3.56 -> full");
    CHECK(StereoDetect::ChooseFullOrHalf(1920.0f / 1080.0f, amb) == StereoLayout::SbsHalf,
          "full/half: 1.78 -> half");
    CHECK(StereoDetect::ChooseFullOrHalf(3.2f, amb) == StereoLayout::SbsFull,
          "full/half: 3.2 -> full (hard prior)");
    CHECK(StereoDetect::ChooseFullOrHalf(1.3333f, amb) == StereoLayout::SbsHalf,
          "full/half: 4:3 -> half (hard prior)");
    StereoDetect::ChooseFullOrHalf(2.0f, amb);
    CHECK(amb, "full/half: 2.0 is genuinely ambiguous and must say so");
}

// The regression lock demanded by "the filename convention must keep working
// byte-for-byte". If any of these change, the naming convention has been broken.
void TestFilenameUnchanged() {
    MediaInfo a = MediaSource::Identify("clip_half_2x1.mp4");
    CHECK(a.layout == StereoLayout::SbsHalf, "filename: *_half_2x1 -> SBS-half");
    CHECK(a.signal == StereoSignal::Filename, "filename: signal");
    CHECK(a.kind == MediaKind::Video, "filename: .mp4 -> video");

    MediaInfo b = MediaSource::Identify("shot_2x1.png");
    CHECK(b.layout == StereoLayout::SbsFull, "filename: *_2x1 -> SBS-full");
    CHECK(b.signal == StereoSignal::Filename, "filename: signal");

    MediaInfo c = MediaSource::Identify("wide.png", 3840, 1080);
    CHECK(c.layout == StereoLayout::SbsFull, "aspect: >=1.9 unsuffixed -> SBS-full");
    CHECK(c.signal == StereoSignal::Aspect, "aspect: signal");

    MediaInfo d = MediaSource::Identify("plain.png", 1920, 1080);
    CHECK(d.layout == StereoLayout::Mono, "aspect: 16:9 unsuffixed -> mono");
    CHECK(d.signal == StereoSignal::Default, "aspect: signal");

    CHECK(MediaSource::IsSupported("a.MPO"), "supported: .mpo (case-insensitive)");
    CHECK(MediaSource::IsSupported("a.lif") && MediaSource::IsSupported("a.mkv"),
          "supported: existing set intact");
    CHECK(!MediaSource::IsSupported("notes.txt"), "supported: rejects other types");
}

// One assert per rung of the ladder.
void TestResolvePriority() {
    const StereoLayout manual = StereoLayout::Mono;
    MediaInfo meta;
    meta.layout = StereoLayout::SbsHalf;
    meta.eyeSwap = true;
    StereoDetectResult content;
    content.decided = true;
    content.layout = StereoLayout::SbsFull;
    content.confidence = 0.9f;

    // 1. manual beats metadata, filename, content and aspect all at once.
    MediaInfo r1 = MediaSource::Resolve("x_2x1.png", MediaKind::Image, 3840, 1080,
                                        &manual, &meta, &content);
    CHECK(r1.layout == StereoLayout::Mono && r1.signal == StereoSignal::Manual,
          "resolve: manual wins");

    // 2. metadata beats filename and content — and is the only source of eyeSwap.
    MediaInfo r2 = MediaSource::Resolve("x_2x1.png", MediaKind::Image, 3840, 1080,
                                        nullptr, &meta, &content);
    CHECK(r2.layout == StereoLayout::SbsHalf && r2.signal == StereoSignal::Metadata,
          "resolve: metadata beats filename");
    CHECK(r2.eyeSwap, "resolve: eyeSwap comes from metadata");

    // 3. filename beats content.
    MediaInfo r3 = MediaSource::Resolve("x_half_2x1.png", MediaKind::Image, 3840, 1080,
                                        nullptr, nullptr, &content);
    CHECK(r3.layout == StereoLayout::SbsHalf && r3.signal == StereoSignal::Filename,
          "resolve: filename beats content");

    // 4. content beats aspect.
    MediaInfo r4 = MediaSource::Resolve("x.png", MediaKind::Image, 3840, 1080,
                                        nullptr, nullptr, &content);
    CHECK(r4.signal == StereoSignal::Content, "resolve: content beats aspect");

    // 4b. an UNDECIDED content result must not be consulted at all.
    StereoDetectResult undecided;
    undecided.decided = false;
    MediaInfo r5 = MediaSource::Resolve("x.png", MediaKind::Image, 3840, 1080,
                                        nullptr, nullptr, &undecided);
    CHECK(r5.signal == StereoSignal::Aspect && r5.layout == StereoLayout::SbsFull,
          "resolve: undecided content falls through to aspect");

    // 5. nothing at all -> mono.
    MediaInfo r6 = MediaSource::Resolve("x.png", MediaKind::Image, 1920, 1080,
                                        nullptr, nullptr, nullptr);
    CHECK(r6.layout == StereoLayout::Mono && r6.signal == StereoSignal::Default,
          "resolve: default is mono");
}

} // namespace

int main() {
    std::printf("stereo_detect_test\n");
    TestSbsFull();
    TestSbsHalf();
    TestMono16x9();
    TestMono2x1();
    TestMono21x9();
    TestLetterboxedSbs();
    TestPillarboxedMono();
    TestPillarboxedSbs();
    TestFlatAbstains();
    TestFadeBlackAbstains();
    TestPeriodicRejected();
    TestGrossDisparityRejected();
    TestRgbaMatchesLuma();
    TestRowStride();
    TestPixelStride10Bit();
    TestOddWidth();
    TestVote();
    TestChooseFullOrHalf();
    TestFilenameUnchanged();
    TestResolvePriority();

    if (g_failures == 0) {
        std::printf("  all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "  %d check(s) failed\n", g_failures);
    return 1;
}
