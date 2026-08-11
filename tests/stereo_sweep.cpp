// SPDX-License-Identifier: Apache-2.0
//
// stereo_sweep — run the layout decision over the generated asset corpus and check every
// file against its expected verdict (#45).
//
// The unit tests cover the detector on synthetic buffers; this covers the whole ladder on
// real encoded media — JPEG/PNG through stb, H.264 through the probe, MKV StereoMode and
// H.264 frame-packing SEI through AVStereo3D, and the MPO container — which is the only
// way to check that the layers compose in the right order on real files.
//
// It needs no Vulkan, no OpenXR and no display, so it runs anywhere the player builds.
//
// Corpus: scripts/gen_stereo_test_assets.sh writes assets/media/detect/ (gitignored). When
// that directory is absent this exits 0 with a notice, so CI stays green without it.
//
// NOTE: the dispatch below deliberately mirrors App::LoadMedia's ladder — kind, then
// container, then the probe/detector, then Resolve. If that order changes in App.cpp,
// change it here too; the duplication is the price of checking the decision without
// standing up a renderer.

#include "media/ImageDecoder.h"
#include "media/LifLoader.h"
#include "media/MediaSource.h"
#include "media/MpoLoader.h"
#include "media/StereoDetect.h"
#include "media/VideoStereoProbe.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace mp;

namespace {

struct Expect {
    const char* file;
    StereoLayout layout;
    StereoSignal signal;
    bool eyeSwap;
    const char* why;
};

// The acceptance table. Every row is a case the shipped heuristic got wrong, or a
// behaviour that must not regress.
const Expect kExpected[] = {
    {"sbs_full_unsuffixed.png", StereoLayout::SbsFull, StereoSignal::Content, false,
     "unsuffixed full SBS — only the pixels can say"},
    {"sbs_half_unsuffixed.png", StereoLayout::SbsHalf, StereoSignal::Content, false,
     "unsuffixed half SBS — dimensionally identical to mono 16:9"},
    {"mono_16x9.png", StereoLayout::Mono, StereoSignal::Content, false,
     "plain mono"},
    {"mono_21x9.png", StereoLayout::Mono, StereoSignal::Content, false,
     "21:9 panorama — the old >=1.9 rule split this"},
    {"mono_2to1.png", StereoLayout::Mono, StereoSignal::Content, false,
     "2:1 photo — the old >=1.9 rule split this"},
    {"mono_letterbox_16x9.png", StereoLayout::Mono, StereoSignal::Content, false,
     "letterboxed mono — matching bars must not fake a match"},
    {"mono_pillarbox_2to1.png", StereoLayout::Mono, StereoSignal::Content, false,
     "pillarboxed mono — aspect says SBS and the bars match"},
    {"sbs_full_letterboxed.png", StereoLayout::SbsFull, StereoSignal::Content, false,
     "letterboxed SBS — detect through the bars"},
    {"sbs_full_pillarbox.png", StereoLayout::SbsFull, StereoSignal::Content, false,
     "pillarboxed SBS — needs the per-half crop"},
    {"mono_but_named_2x1.png", StereoLayout::SbsFull, StereoSignal::Filename, false,
     "filename convention must still beat the content detector"},
    {"flat_lowtexture_2to1.png", StereoLayout::SbsFull, StereoSignal::Aspect, false,
     "near-flat: detector abstains, aspect is the documented last resort"},
    {"sbs_full_video.mp4", StereoLayout::SbsFull, StereoSignal::Content, false,
     "unsuffixed SBS video"},
    {"sbs_fadein_video.mp4", StereoLayout::SbsFull, StereoSignal::Content, false,
     "SBS video fading in from black — frame 0 must not decide"},
    {"sbs_meta_lr.mkv", StereoLayout::SbsFull, StereoSignal::Metadata, false,
     "Matroska StereoMode left_right"},
    {"sbs_meta_rl.mkv", StereoLayout::SbsFull, StereoSignal::Metadata, true,
     "Matroska StereoMode right_left — must set the eye-swap hint"},
    {"sbs_meta_sei.mp4", StereoLayout::SbsFull, StereoSignal::Metadata, false,
     "H.264 frame-packing SEI — invisible to the stream-level API"},
    {"stereo_pair.mpo", StereoLayout::SbsFull, StereoSignal::Metadata, false,
     "MPO container"},
};

bool HasExt(const std::string& p, const char* ext) {
    const size_t n = std::strlen(ext);
    if (p.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const char a = (char)(p[p.size() - n + i] | 0x20);
        if (a != ext[i]) return false;
    }
    return true;
}

// Mirrors App::LoadMedia's resolution ladder. Returns the MediaInfo the app would apply.
// `det` receives the raw detector verdict so a failure can explain itself.
MediaInfo ResolveFile(const std::string& path, StereoDetectResult& det) {
    const MediaInfo probe = MediaSource::Identify(path);
    StereoLayout fnLayout = StereoLayout::Mono;
    const bool fnDecided = MediaSource::LayoutFromFilename(path, fnLayout);
    const bool wantContent = !fnDecided;

    if (probe.kind == MediaKind::Video) {
        VideoStereoProbe::Result pr = VideoStereoProbe::Run(path, wantContent);
        det = pr.content;
        MediaInfo meta;
        if (pr.haveMeta) {
            meta.layout = pr.metaLayout;
            meta.eyeSwap = pr.metaInvert;
            if (meta.layout != StereoLayout::Mono && pr.width > 0 && pr.height > 0) {
                bool amb = false;
                meta.layout = StereoDetect::ChooseFullOrHalf(
                    (float)pr.width / (float)pr.height, amb);
            }
        }
        return MediaSource::Resolve(path, MediaKind::Video, pr.ok ? pr.width : 0,
                                    pr.ok ? pr.height : 0, nullptr,
                                    pr.haveMeta ? &meta : nullptr,
                                    pr.content.decided ? &pr.content : nullptr);
    }

    if (HasExt(path, ".lif") || LifLoader::IsLif(path)) {
        LifResult lif = LifLoader::Load(path);
        if (lif.ok)
            return MediaInfo{MediaKind::Image, lif.layout, StereoSignal::Metadata, false, 1.0f};
    }
    if (HasExt(path, ".mpo") || MpoLoader::IsMpo(path)) {
        MpoResult m = MpoLoader::Load(path);
        if (m.ok)
            return MediaInfo{MediaKind::Image, m.layout, StereoSignal::Metadata, false, 1.0f};
    }
    DecodedImage img = ImageDecoder::Load(path);
    if (!img.Valid()) return MediaInfo{};
    StereoDetectResult content;
    if (wantContent)
        content = StereoDetect::AnalyzeRGBA(img.pixels.data(), img.width, img.height,
                                            (ptrdiff_t)img.width * 4);
    det = content;
    return MediaSource::Resolve(path, MediaKind::Image, img.width, img.height, nullptr,
                                nullptr, content.decided ? &content : nullptr);
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "assets/media/detect";
    if (!std::filesystem::is_directory(dir)) {
        std::printf("stereo_sweep: '%s' not present — run scripts/gen_stereo_test_assets.sh\n",
                    dir.c_str());
        std::printf("  (skipping; this is not a failure)\n");
        return 0;
    }

    int failures = 0, checked = 0, missing = 0;
    std::printf("stereo_sweep: %s\n\n", dir.c_str());
    std::printf("  %-28s %-9s %-18s %s\n", "asset", "layout", "signal", "verdict");
    std::printf("  %-28s %-9s %-18s %s\n", "----------------------------", "---------",
                "------------------", "-------");

    for (const Expect& e : kExpected) {
        const std::string path = (std::filesystem::path(dir) / e.file).string();
        if (!std::filesystem::exists(path)) {
            std::printf("  %-28s %-9s %-18s MISSING\n", e.file, "-", "-");
            ++missing;
            continue;
        }
        StereoDetectResult det;
        const MediaInfo got = ResolveFile(path, det);
        ++checked;
        const bool ok = got.layout == e.layout && got.signal == e.signal &&
                        got.eyeSwap == e.eyeSwap;
        if (!ok) ++failures;
        std::printf("  %-28s %-9s %-18s %s\n", e.file, LayoutName(got.layout),
                    SignalName(got.signal), ok ? "ok" : "FAIL");
        if (!ok) {
            std::printf("      expected %s / %s%s  (%s)\n", LayoutName(e.layout),
                        SignalName(e.signal), e.eyeSwap ? " / eyes R|L" : "", e.why);
            if (got.eyeSwap != e.eyeSwap)
                std::printf("      eye-swap hint: got %s\n", got.eyeSwap ? "R|L" : "L|R");
            std::printf("      detector: %s peak=%.3f base=%.3f margin=%.3f d=%d "
                        "sigma=%.1f grad=%.2f (%s)\n",
                        det.abstained ? "ABSTAIN" : (det.decided ? "decided" : "undecided"),
                        (double)det.peak, (double)det.baseline, (double)det.margin,
                        det.disparityPx, (double)det.sigmaMin, (double)det.gradMin,
                        det.reason);
        }
    }

    std::printf("\n  %d checked, %d failed, %d missing\n", checked, failures, missing);
    return failures == 0 ? 0 : 1;
}
