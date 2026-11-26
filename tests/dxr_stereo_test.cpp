// SPDX-License-Identifier: Apache-2.0
//
// DisplayXR Camera files in the player (src/media/DxrStereo.h): the metadata the player
// reads and the one sign conversion it makes.
//
// The test files are made by the format's own reference WRITER (third_party/dxrmedia:
// write_still, write_sbs_still, mp4_finalize, encode_frame_meta), from JPEGs encoded here
// with stb_image_write, exactly as the camera assembles its files. No fixtures.

#include "media/AutoConvergence.h"  // ConvergenceShiftPx, the draw code's own shift
#include "media/DxrStereo.h"
#include "media/DxrVideoTrack.h"
#include "media/MpoLoader.h"

#include "dxrmedia/stills.h"
#include "dxrmedia/video.h"
#include "mp4_builder.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

bool Near(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps; }

void AppendBytes(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* p = static_cast<const uint8_t*>(data);
    v->insert(v->end(), p, p + size);
}

// A textured view: `red` picks a red- or blue-dominant palette (so the eyes can be told
// apart after composing), `phase` shifts the texture horizontally (a fake disparity).
std::vector<uint8_t> MakeJpeg(int w, int h, bool red, int phase) {
    std::vector<uint8_t> px((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int u = x + phase;
            const uint8_t t = (uint8_t)((((u / 6) + (y / 6)) & 1) ? 90 : 0) + (uint8_t)((u * 7 + y * 3) % 60);
            uint8_t* p = &px[((size_t)y * w + x) * 3];
            p[0] = red ? (uint8_t)(160 + t / 2) : (uint8_t)(t / 4);
            p[1] = (uint8_t)(t / 3);
            p[2] = red ? (uint8_t)(t / 4) : (uint8_t)(160 + t / 2);
        }
    std::vector<uint8_t> out;
    stbi_write_jpg_to_func(AppendBytes, &out, w, h, 3, px.data(), 92);
    return out;
}

std::string OutPath(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "dxr_stereo_test";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
    return (bool)f;
}

// Replace every occurrence of `from` by `to` (same length) in `b`.
int Patch(std::vector<uint8_t>& b, const char* from, const char* to) {
    const size_t n = std::strlen(from);
    int hits = 0;
    for (size_t i = 0; i + n <= b.size(); ++i)
        if (std::memcmp(&b[i], from, n) == 0) {
            std::memcpy(&b[i], to, n);
            ++hits;
        }
    return hits;
}

constexpr int kEyeW = 160, kEyeH = 96;

dxrmedia::StillMeta CameraMeta(bool withConvergence, double convergence) {
    dxrmedia::StillMeta m;
    m.exif.make = "TestMake";
    m.exif.model = "TestModel";
    m.stereo.rectified = true;
    m.stereo.alignment = "parallel";
    if (withConvergence) m.stereo.convergence = convergence;
    m.stereo.convergenceSource = "auto";
    m.stereo.baselineMm = 40.0;
    m.stereo.facing = "rear";
    return m;
}

// --- the one conversion ----------------------------------------------------------------

void TestSign() {
    std::printf("sign: file disparity -> player shift\n");
    CHECK(Near(mp::dxrfile::ShiftForDisparity(0.04f), -0.02f), "d = 0.04 -> s = -0.02");
    CHECK(Near(mp::dxrfile::ShiftForDisparity(0.0f), 0.0f), "infinity stays at infinity");
    CHECK(Near(mp::dxrfile::ShiftForDisparity(-0.01f), 0.005f), "a negative d flips");
    // The draw code: a point of file disparity d (x_left - x_right) is shown at
    // (x_left + shiftL) - (x_right + shiftR); with the converted shift that is 0, i.e.
    // the suggested plane lands on the screen.
    for (const float d : {0.0f, 0.012f, 0.04f, 0.11f}) {
        const uint32_t W = 2000;
        const float s = mp::dxrfile::ShiftForDisparity(d);
        const int32_t shiftL = mp::autoconv::ConvergenceShiftPx(s, W, true);
        const int32_t shiftR = mp::autoconv::ConvergenceShiftPx(s, W, false);
        const float shown = d * (float)W + (float)(shiftL - shiftR);
        CHECK(std::fabs(shown) <= 1.0f, "the suggested plane is shown at zero parallax");
    }
}

// --- stills ----------------------------------------------------------------------------

void TestMpfStill() {
    std::printf("still: MPF pair with dxr XMP\n");
    const std::vector<uint8_t> left = MakeJpeg(kEyeW, kEyeH, /*red=*/true, 0);
    const std::vector<uint8_t> right = MakeJpeg(kEyeW, kEyeH, /*red=*/false, 6);
    dxrmedia::StillInputs in;
    in.left = left;
    in.right = right;
    std::vector<uint8_t> file;
    std::string err;
    CHECK(dxrmedia::write_still(in, CameraMeta(true, 0.04), file, &err), "the reference writer made the still");

    const mp::dxrfile::StillInfo info = mp::dxrfile::ReadStill(file.data(), file.size());
    CHECK(info.ours, "ReadStill: ours");
    CHECK(info.pair && !info.sbs, "ReadStill: an MPF pair");
    CHECK(info.hasConvergence && Near(info.shift, -0.02f), "ReadStill: shift = -Convergence/2");
    CHECK(info.eyeWidth == kEyeW && info.eyeHeight == kEyeH, "ReadStill: eye size");

    const std::string path = OutPath("DXR_20261125_101112.jpg");
    CHECK(WriteFile(path, file), "wrote the still");
    CHECK(mp::MpoLoader::IsMpo(path), "the MPO sniff takes it");
    const mp::MpoResult r = mp::MpoLoader::Load(path);
    CHECK(r.ok && r.stereo && r.layout == mp::StereoLayout::SbsFull, "MpoLoader composes the pair");
    CHECK(r.image.width == 2 * kEyeW && r.image.height == kEyeH, "full SBS, two eyes wide");
    CHECK(r.dxr && r.fileConvergence, "MpoLoader: the file's own convergence");
    CHECK(Near(r.autoConvergence, -0.02f), "MpoLoader: the starting shift is the file's, converted once");
    if (r.ok) {
        // Left first: the red view is the left half, the blue one the right half.
        auto at = [&](int x, int y) { return &r.image.pixels[((size_t)y * r.image.width + x) * 4]; };
        const uint8_t* l = at(kEyeW / 2, kEyeH / 2);
        const uint8_t* rr = at(kEyeW + kEyeW / 2, kEyeH / 2);
        CHECK(l[0] > l[2] + 60, "the left half is the primary (left) view");
        CHECK(rr[2] > rr[0] + 60, "the right half is MPF image 1 (right) view");
    }

    // Convergence is optional: a camera still without it falls back to the estimate.
    std::vector<uint8_t> noConv;
    CHECK(dxrmedia::write_still(in, CameraMeta(false, 0.0), noConv, &err), "still without convergence");
    const std::string pathNoConv = OutPath("DXR_noconv.jpg");
    WriteFile(pathNoConv, noConv);
    const mp::MpoResult rn = mp::MpoLoader::Load(pathNoConv);
    CHECK(rn.ok && rn.dxr && !rn.fileConvergence, "no dxr:Convergence: measured instead");
    // An independent check of the sign: the player's own estimator, measuring these pixels
    // (the right view's texture is 6 px left of the left view's: d = +6 / 160), lands on
    // the same shift the conversion gives for that disparity.
    CHECK(Near(rn.autoConvergence, mp::dxrfile::ShiftForDisparity(6.0f / (float)kEyeW), 0.002f),
          "the estimator and the conversion agree on the sign");

    // Not ours: the same bytes with the namespace URI broken behave as any MPO did before.
    std::vector<uint8_t> foreign = file;
    CHECK(Patch(foreign, "http://ns.displayxr.org/stereo/1.0/", "http://ns.example.invalid/stereo/1/") >= 1,
          "patched the namespace");
    const mp::dxrfile::StillInfo fi = mp::dxrfile::ReadStill(foreign.data(), foreign.size());
    CHECK(!fi.ours && !fi.hasConvergence, "a foreign namespace is not ours");
    const std::string pathForeign = OutPath("foreign.jpg");
    WriteFile(pathForeign, foreign);
    const mp::MpoResult rf = mp::MpoLoader::Load(pathForeign);
    CHECK(rf.ok && !rf.dxr && !rf.fileConvergence, "a foreign MPO: no file convergence");
    CHECK(Near(rf.autoConvergence, rn.autoConvergence), "a foreign MPO converges by the estimate, as before");
}

void TestSbsStill() {
    std::printf("still: side-by-side export (sbs-lr) and a plain JPEG\n");
    const std::vector<uint8_t> sbs = MakeJpeg(2 * kEyeW, kEyeH, true, 0);
    std::vector<uint8_t> file;
    std::string err;
    CHECK(dxrmedia::write_sbs_still(sbs, CameraMeta(true, 0.03), file, &err), "the reference writer made the export");
    const mp::dxrfile::StillInfo info = mp::dxrfile::ReadStill(file.data(), file.size());
    CHECK(info.ours && info.sbs && !info.pair, "ReadStill: sbs-lr");
    CHECK(info.hasConvergence && Near(info.shift, -0.015f), "ReadStill: the export's shift");
    const std::string path = OutPath("DXR_20261125_101112_sbs.jpg");
    WriteFile(path, file);
    const mp::dxrfile::StillInfo fromFile = mp::dxrfile::ReadStillFile(path);
    CHECK(fromFile.ours && fromFile.sbs && Near(fromFile.shift, -0.015f), "ReadStillFile agrees");
    CHECK(!mp::MpoLoader::IsMpo(path), "the export has no MPF: not an MPO");

    const mp::dxrfile::StillInfo plain = mp::dxrfile::ReadStill(sbs.data(), sbs.size());
    CHECK(!plain.ours && !plain.sbs && !plain.hasConvergence, "a plain JPEG is not ours");
    const mp::dxrfile::StillInfo junk = mp::dxrfile::ReadStill(reinterpret_cast<const uint8_t*>("not a jpeg"), 10);
    CHECK(!junk.ours, "garbage is not ours");
}

// --- video -----------------------------------------------------------------------------

std::vector<uint8_t> FrameSample(bool valid, float convergence, bool cut = false) {
    dxrmedia::FrameMeta f;
    f.convergenceValid = valid;
    f.convergence = convergence;
    f.sceneCut = cut;
    const auto e = dxrmedia::encode_frame_meta(f);
    return std::vector<uint8_t>(e.begin(), e.end());
}

void TestCurve() {
    std::printf("video: per-frame convergence curve\n");
    mp::dxrfile::ConvergenceCurve c;
    CHECK(c.Empty() && Near(c.ShiftAt(0, 0.25f), 0.25f), "empty: the fallback");
    auto add = [&](int64_t pts, bool valid, float conv, bool cut = false) {
        const auto s = FrameSample(valid, conv, cut);
        return c.Add(pts, s.data(), s.size());
    };
    CHECK(add(0, true, 0.02f), "sample 0");
    CHECK(add(200000, true, 0.06f), "sample 2 (out of order)");
    CHECK(add(100000, true, 0.04f), "sample 1");
    CHECK(!add(150000, false, 0.5f), "a sample without the valid bit is skipped");
    CHECK(!add(160000, true, std::nanf("")), "a non-finite convergence is skipped");
    CHECK(c.Size() == 3, "three valid samples kept");
    CHECK(Near(c.ShiftAt(0, 9.f), -0.01f), "at a sample: -d/2");
    CHECK(Near(c.ShiftAt(50000, 9.f), -0.015f), "between samples: linear");
    CHECK(Near(c.ShiftAt(150000, 9.f), -0.025f), "between the reordered samples");
    CHECK(Near(c.ShiftAt(-5, 9.f), -0.01f) && Near(c.ShiftAt(900000, 9.f), -0.03f), "held outside the range");
    CHECK(add(300000, true, 0.10f, /*cut=*/true), "a scene cut");
    CHECK(Near(c.ShiftAt(250000, 9.f), -0.03f), "no interpolation into a cut: the old shot holds");
    CHECK(Near(c.ShiftAt(300000, 9.f), -0.05f), "the cut frame has its own value");
    // A shorter-than-32-byte sample is not a sample.
    const auto s = FrameSample(true, 0.01f);
    CHECK(!c.Add(400000, s.data(), 31), "a 31-byte sample is refused");
}

void TestVideo() {
    std::printf("video: *_sbs.mp4 with the XMP box and the metadata track\n");
    // Ten frames at 30 fps, the camera's track carrying d = 0.01 * i (mp4_builder).
    const auto tracks = mp4b::two_tracks(10);
    const std::vector<uint8_t> raw = mp4b::build_mp4(tracks, {});
    const std::string plain = OutPath("plain.mp4");
    WriteFile(plain, raw);
    CHECK(!mp::dxrfile::ReadVideo(plain).ours, "an mp4 without the XMP box is not ours");
    CHECK(!mp::dxrfile::ReadVideo(OutPath("does_not_exist.mp4")).ours, "a missing file is not ours");

    const std::string path = OutPath("DXR_20261125_101112_sbs.mp4");
    WriteFile(path, raw);
    dxrmedia::StereoMeta xmp = CameraMeta(true, 0.03).stereo;
    xmp.eyeWidth = 320;
    xmp.eyeHeight = 180;
    dxrmedia::Mp4FinalizeOptions opt;
    opt.xmp = &xmp;
    std::string err;
    CHECK(dxrmedia::mp4_finalize(path, opt, nullptr, &err), "the reference post-pass finalised the clip");

    const mp::dxrfile::VideoInfo v = mp::dxrfile::ReadVideo(path);
    CHECK(v.ours && v.sbs, "ReadVideo: ours, sbs-lr");
    CHECK(v.hasConvergence && Near(v.shift, -0.015f), "ReadVideo: the static shift");
    CHECK(v.metaTrackId == 2 && v.metaSamples == 10, "ReadVideo: the metadata track");
#if !defined(_WIN32)
    const int fd = ::open(path.c_str(), O_RDONLY);
    const mp::dxrfile::VideoInfo vf = mp::dxrfile::ReadVideoFd(fd);
    CHECK(vf.ours && vf.sbs && Near(vf.shift, -0.015f) && vf.metaTrackId == 2, "ReadVideoFd agrees");
    if (fd >= 0) ::close(fd);
#endif

#if MEDIAPLAYER_WITH_FFMPEG
    mp::dxrfile::ConvergenceCurve curve;
    const int n = mp::dxrfile::ReadConvergenceTrack(path, v.metaTrackId, curve);
    CHECK(n == 10 && curve.Size() == 10, "FFmpeg read the whole metadata track");
    // Frame i is at i/30 s and carries d = 0.01 i.
    for (int i = 0; i < 10; ++i) {
        const int64_t pts = (int64_t)std::llround(i * 1e6 / 30.0);
        CHECK(Near(curve.ShiftAt(pts, 9.f), -0.005f * (float)i, 1e-5f), "frame i: -d_i/2 at its own timestamp");
    }
    mp::dxrfile::ConvergenceCurve none;
    CHECK(mp::dxrfile::ReadConvergenceTrack(path, 77, none) == 0 && none.Empty(), "a wrong track id reads nothing");
#else
    std::printf("  (FFmpeg not built in: the per-frame demux is not checked here)\n");
#endif
}

}  // namespace

int main() {
    TestSign();
    TestMpfStill();
    TestSbsStill();
    TestCurve();
    TestVideo();
    if (g_failures) {
        std::fprintf(stderr, "dxr_stereo_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("dxr_stereo_test: all %d checks passed\n", g_checks);
    return 0;
}
