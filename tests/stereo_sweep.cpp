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
    StereoLayout layout;      // expected under the DEFAULT policy (detection off)
    StereoSignal signal;
    StereoLayout fullLayout;  // expected with MEDIAPLAYER_STEREO_DETECT=full
    StereoSignal fullSignal;
    bool eyeSwap;
    const char* why;
};

// The acceptance table, checked under BOTH policies.
//
// Default ("meta"): filename, then container metadata, then ASSUME STEREO and let the
// aspect settle full-vs-half. This player targets a 3D display; mono is not a use case,
// so an unidentified file is taken to be stereo. Note what that buys and what it costs
// in the rows below: every real SBS file resolves correctly with zero tuning, and every
// mono file is mis-packed -- knowingly.
//
// Opt-in ("full") additionally runs the pixel detector, which is the only layer able to
// conclude MONO. The two columns together are the honest statement of the trade.
const Expect kExpected[] = {
    {"sbs_full_unsuffixed.png", StereoLayout::SbsFull, StereoSignal::Aspect,
     StereoLayout::SbsFull, StereoSignal::Content, false,
     "unsuffixed full SBS"},
    {"sbs_half_unsuffixed.png", StereoLayout::SbsHalf, StereoSignal::Aspect,
     StereoLayout::SbsHalf, StereoSignal::Content, false,
     "unsuffixed half SBS — the case a threshold cannot reach"},
    {"mono_16x9.png", StereoLayout::SbsHalf, StereoSignal::Aspect,
     StereoLayout::Mono, StereoSignal::Content, false,
     "mono: mis-packed by default (accepted), rescued by the detector"},
    {"mono_21x9.png", StereoLayout::SbsHalf, StereoSignal::Aspect,
     StereoLayout::Mono, StereoSignal::Content, false,
     "mono panorama: same trade"},
    {"mono_2to1.png", StereoLayout::SbsFull, StereoSignal::Aspect,
     StereoLayout::Mono, StereoSignal::Content, false,
     "2:1 mono: same trade"},
    {"mono_letterbox_16x9.png", StereoLayout::SbsHalf, StereoSignal::Aspect,
     StereoLayout::Mono, StereoSignal::Content, false,
     "letterboxed mono: same trade"},
    {"mono_pillarbox_2to1.png", StereoLayout::SbsFull, StereoSignal::Aspect,
     StereoLayout::Mono, StereoSignal::Content, false,
     "pillarboxed mono: same trade"},
    {"sbs_full_letterboxed.png", StereoLayout::SbsFull, StereoSignal::Aspect,
     StereoLayout::SbsFull, StereoSignal::Content, false,
     "letterboxed SBS"},
    {"sbs_full_pillarbox.png", StereoLayout::SbsFull, StereoSignal::Aspect,
     StereoLayout::SbsFull, StereoSignal::Content, false,
     "pillarboxed SBS"},
    {"mono_but_named_2x1.png", StereoLayout::SbsFull, StereoSignal::Filename,
     StereoLayout::SbsFull, StereoSignal::Filename, false,
     "filename beats everything below it"},
    {"flat_lowtexture_2to1.png", StereoLayout::SbsFull, StereoSignal::Aspect,
     StereoLayout::SbsFull, StereoSignal::Aspect, false,
     "near-flat: detector abstains, aspect policy applies either way"},
    {"sbs_full_video.mp4", StereoLayout::SbsFull, StereoSignal::Aspect,
     StereoLayout::SbsFull, StereoSignal::Content, false,
     "unsuffixed SBS video"},
    {"sbs_fadein_video.mp4", StereoLayout::SbsFull, StereoSignal::Aspect,
     StereoLayout::SbsFull, StereoSignal::Content, false,
     "SBS video fading in from black"},
    {"sbs_meta_lr.mkv", StereoLayout::SbsFull, StereoSignal::Metadata,
     StereoLayout::SbsFull, StereoSignal::Metadata, false,
     "Matroska StereoMode left_right"},
    {"sbs_meta_rl.mkv", StereoLayout::SbsFull, StereoSignal::Metadata,
     StereoLayout::SbsFull, StereoSignal::Metadata, true,
     "Matroska StereoMode right_left — eye-swap hint"},
    {"sbs_meta_sei.mp4", StereoLayout::SbsFull, StereoSignal::Metadata,
     StereoLayout::SbsFull, StereoSignal::Metadata, false,
     "H.264 frame-packing SEI"},
    {"stereo_pair.mpo", StereoLayout::SbsFull, StereoSignal::Metadata,
     StereoLayout::SbsFull, StereoSignal::Metadata, false,
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
// `useContent` mirrors MEDIAPLAYER_STEREO_DETECT=full; `det` receives the raw detector
// verdict so a failure can explain itself.
MediaInfo ResolveFile(const std::string& path, bool useContent, StereoDetectResult& det) {
    det = StereoDetectResult{};
    const MediaInfo probe = MediaSource::Identify(path);
    StereoLayout fnLayout = StereoLayout::Mono;
    const bool fnDecided = MediaSource::LayoutFromFilename(path, fnLayout);
    const bool wantContent = useContent && !fnDecided;

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

int RunPolicy(const std::string& dir, bool useContent, int& missing) {
    int failures = 0, checked = 0;
    std::printf("\n  === policy: %s ===\n",
                useContent ? "MEDIAPLAYER_STEREO_DETECT=full (opt-in pixel detector)"
                           : "default (filename / metadata / assume-stereo-from-aspect)");
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
        const StereoLayout wantLayout = useContent ? e.fullLayout : e.layout;
        const StereoSignal wantSignal = useContent ? e.fullSignal : e.signal;
        StereoDetectResult det;
        const MediaInfo got = ResolveFile(path, useContent, det);
        ++checked;
        const bool ok = got.layout == wantLayout && got.signal == wantSignal &&
                        got.eyeSwap == e.eyeSwap;
        if (!ok) ++failures;
        std::printf("  %-28s %-9s %-18s %s\n", e.file, LayoutName(got.layout),
                    SignalName(got.signal), ok ? "ok" : "FAIL");
        if (!ok) {
            std::printf("      expected %s / %s%s  (%s)\n", LayoutName(wantLayout),
                        SignalName(wantSignal), e.eyeSwap ? " / eyes R|L" : "", e.why);
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

    std::printf("\n  %d checked, %d failed\n", checked, failures);
    return failures;
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
    std::printf("stereo_sweep: %s\n", dir.c_str());

    int missing = 0;
    int failures = RunPolicy(dir, /*useContent=*/false, missing);
    failures += RunPolicy(dir, /*useContent=*/true, missing);
    if (missing) std::printf("\n  %d asset(s) missing\n", missing);
    return failures == 0 ? 0 : 1;
}
