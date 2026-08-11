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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace mp;

namespace {

struct Expect {
    const char* file;
    StereoLayout layout;      // expected with MEDIAPLAYER_STEREO_DETECT=meta (no detector)
    StereoSignal signal;
    StereoLayout fullLayout;  // expected under the DEFAULT policy (detector on)
    StereoSignal fullSignal;
    bool eyeSwap;
    const char* why;
};

// The acceptance table, checked under BOTH policies.
//
// DEFAULT: filename, container metadata, then the pixel detector, then ASSUME STEREO.
// The detector may only conclude MONO on unambiguous evidence; anything short of that
// falls through to the stereo assumption, because this player targets a 3D display and
// mono is not a use case.
//
// The second column is MEDIAPLAYER_STEREO_DETECT=meta, i.e. the same ladder with the
// detector switched off, which shows what the stereo assumption does on its own: every
// real SBS file still resolves correctly, and every mono file is mis-packed. The two
// columns together are the honest statement of what the detector buys.
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
mp::StereoDetectParams g_params;   // SWEEP_PREFILTER overrides the high-pass, for tuning

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
                                            (ptrdiff_t)img.width * 4, g_params);
    det = content;
    return MediaSource::Resolve(path, MediaKind::Image, img.width, img.height, nullptr,
                                nullptr, content.decided ? &content : nullptr);
}


// --- corpus scan -------------------------------------------------------------------
//
// Runs the detector over a directory of REAL full-SBS photographs and reports the
// measured distributions, plus two derived cases the same files give us for free:
//
//   sbs-full : the file as shipped              -> must NOT be called mono
//   mono     : the LEFT EYE alone               -> a genuine mono photograph
//   sbs-half : each half squeezed 2x and re-joined -> genuine half-SBS
//
// This is what calibrates the thresholds against sky gradients, defocus and vignetting,
// none of which synthetic fixtures reproduce.
// One decode pass, many thresholds. peak and seam do not depend on the thresholds, so
// the whole (monoPeakMax x seamVetoRatio) grid can be evaluated analytically afterwards
// instead of re-decoding 7680x2160 JPEGs once per candidate setting.
struct Sample {
    float peak = 0, seam = 0;
    bool stereoRung = false;   // the strong/sharp/close rung fired: never mono, any threshold
    bool abstained = false;
};

struct Acc {
    int n = 0, mono = 0, sbsFull = 0, sbsHalf = 0, undecided = 0, abstain = 0;
    double peakSum = 0, marginSum = 0, seamSum = 0;
    float peakMin = 1e9f, peakMax = -1e9f, seamMin = 1e9f, seamMax = -1e9f;
    // The decision boundary that actually matters: among the images whose PEAK is low
    // enough to be mono candidates, how strong is the seam? For real stereo this is the
    // number the veto threshold must sit below; for mono it is the number it must sit
    // above. Everything else in this struct is context.
    std::vector<float> candSeam;
    std::vector<Sample> samples;
    void Add(const StereoDetectResult& r) {
        samples.push_back(Sample{r.peak, r.seam,
                                 r.decided && r.layout != StereoLayout::Mono, r.abstained});
        ++n;
        if (r.peak < 0.55f && !r.abstained) candSeam.push_back(r.seam);
        peakSum += r.peak; marginSum += r.margin; seamSum += r.seam;
        peakMin = std::min(peakMin, r.peak);  peakMax = std::max(peakMax, r.peak);
        seamMin = std::min(seamMin, r.seam);  seamMax = std::max(seamMax, r.seam);
        if (r.abstained) { ++abstain; return; }
        if (!r.decided) { ++undecided; return; }
        if (r.layout == StereoLayout::Mono) ++mono;
        else if (r.layout == StereoLayout::SbsFull) ++sbsFull;
        else ++sbsHalf;
    }
    void Report(const char* label, const char* want) const {
        if (n == 0) { std::printf("  %-9s (none)\n", label); return; }
        std::printf("  %-9s n=%-5d peak %.3f [%.3f..%.3f]  margin %+.3f  seam %.2f "
                    "[%.2f..%.2f]\n    -> mono %d | full %d | half %d | undecided %d | "
                    "abstain %d   (want %s)\n",
                    label, n, peakSum / n, (double)peakMin, (double)peakMax,
                    marginSum / n, seamSum / n, (double)seamMin, (double)seamMax,
                    mono, sbsFull, sbsHalf, undecided, abstain, want);
        if (!candSeam.empty()) {
            std::vector<float> c = candSeam;
            std::sort(c.begin(), c.end());
            std::printf("    low-peak candidates: %zu   seam min %.2f  p05 %.2f  "
                        "median %.2f  max %.2f\n",
                        c.size(), (double)c.front(), (double)c[c.size() / 20],
                        (double)c[c.size() / 2], (double)c.back());
        }
    }
};

// Box-downscale the horizontal axis of one half by 2, in place into `dst`.
DecodedImage SqueezeHalves(const DecodedImage& src) {
    const int mid = src.width / 2, hw = mid / 2;
    DecodedImage out;
    if (hw < 8) return out;
    out.width = hw * 2;
    out.height = src.height;
    out.pixels.assign((size_t)out.width * out.height * 4, 0);
    for (int y = 0; y < src.height; ++y)
        for (int half = 0; half < 2; ++half)
            for (int x = 0; x < hw; ++x) {
                const int sx = half * mid + x * 2;
                const uint8_t* a = &src.pixels[((size_t)y * src.width + sx) * 4];
                const uint8_t* b = &src.pixels[((size_t)y * src.width + sx + 1) * 4];
                uint8_t* d = &out.pixels[((size_t)y * out.width + half * hw + x) * 4];
                for (int c = 0; c < 4; ++c) d[c] = (uint8_t)(((int)a[c] + b[c]) / 2);
            }
    return out;
}

DecodedImage LeftEye(const DecodedImage& src) {
    const int mid = src.width / 2;
    DecodedImage out;
    out.width = mid;
    out.height = src.height;
    out.pixels.assign((size_t)mid * src.height * 4, 0);
    for (int y = 0; y < src.height; ++y)
        std::memcpy(&out.pixels[(size_t)y * mid * 4],
                    &src.pixels[(size_t)y * src.width * 4], (size_t)mid * 4);
    return out;
}

// --list <dir>: resolve every image in a directory and print what decided it. No
// expectations, no ground truth — a diagnostic for pointing at a real media folder.
// --dumpmpo <in> <out.png>: write what MpoLoader actually composed, so the extracted
// views can be diffed against an independent MPO reader rather than merely trusted.
int DumpMpo(const std::string& in, const std::string& out) {
    MpoResult r = MpoLoader::Load(in);
    if (!r.ok) { std::printf("MpoLoader declined '%s'\n", in.c_str()); return 1; }
    if (!stbi_write_png(out.c_str(), r.image.width, r.image.height, 4,
                        r.image.pixels.data(), r.image.width * 4)) {
        std::printf("write failed\n");
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", out.c_str(), r.image.width, r.image.height);
    return 0;
}

int ListDir(const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end());
    std::printf("  %-34s %-9s %-24s %6s %6s %7s\n", "file", "layout", "signal",
                "peak", "margin", "seam");
    std::printf("  %-34s %-9s %-24s %6s %6s %7s\n", "----", "------", "------",
                "----", "------", "----");
    for (const std::string& f : files) {
        if (!MediaSource::IsSupported(f)) continue;
        StereoDetectResult det;
        const MediaInfo got = ResolveFile(f, true, det);
        char pk[10] = "     -", mg[10] = "     -", sm[10] = "      -";
        if (det.peak != 0.0f || det.margin != 0.0f) {
            std::snprintf(pk, sizeof(pk), "%6.3f", (double)det.peak);
            std::snprintf(mg, sizeof(mg), "%6.3f", (double)det.margin);
            std::snprintf(sm, sizeof(sm), "%7.2f", (double)det.seam);
        }
        std::printf("  %-34s %-9s %-24s %6s %6s %7s\n",
                    std::filesystem::path(f).filename().string().c_str(),
                    LayoutName(got.layout), SignalName(got.signal), pk, mg, sm);
    }
    return 0;
}

int ScanCorpus(const std::string& dir, int limit) {
    // GROUND TRUTH matters more than sample size here. Two traps in this corpus:
    //
    //  * It contains far more DERIVED data than photographs — depth maps and disparity
    //    visualisations. Those are not stereo pairs, and counting one as an "SBS image
    //    that must never be called mono" would silently corrupt the measurement.
    //  * Source photographs are all full-SBS at 3.56:1, so anything narrower is either
    //    derived or already a single view.
    //
    // Exclude the derived directories by path, then confirm each survivor is actually
    // full-SBS by its decoded aspect.
    static const char* kDerivedDirs[] = {"depth_db", "FoundationStereo_results"};
    std::vector<std::string> files;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string s = it->path().string();
        bool derived = false;
        for (const char* d : kDerivedDirs)
            if (s.find(d) != std::string::npos) derived = true;
        if (derived) continue;
        const std::string low = [&] { std::string t = s; for (auto& c : t) c = (char)tolower(c); return t; }();
        if (low.size() > 4 && (low.rfind(".jpg") == low.size() - 4 ||
                               low.rfind(".png") == low.size() - 4 ||
                               low.rfind(".jpeg") == low.size() - 5))
            files.push_back(s);
    }
    std::sort(files.begin(), files.end());
    // Stride-sample rather than truncate: taking the first N alphabetically would test
    // one or two shoots rather than the whole range of scenes.
    if ((int)files.size() > limit && limit > 0) {
        std::vector<std::string> pick;
        pick.reserve((size_t)limit);
        for (int i = 0; i < limit; ++i)
            pick.push_back(files[(size_t)((int64_t)i * files.size() / limit)]);
        files.swap(pick);
    }
    std::printf("corpus scan: %zu file(s) from %s\n\n", files.size(), dir.c_str());

    Acc full, mono, half;
    int skippedNarrow = 0;
    std::vector<std::string> misses;
    for (const std::string& f : files) {
        DecodedImage img = ImageDecoder::Load(f);
        if (!img.Valid() || img.width < 64) continue;
        // Ground-truth gate: a source frame in this corpus is full-SBS at ~3.56:1.
        if ((float)img.width / (float)img.height < 3.0f) { ++skippedNarrow; continue; }

        StereoDetectResult r = StereoDetect::AnalyzeRGBA(
            img.pixels.data(), img.width, img.height, (ptrdiff_t)img.width * 4, g_params);
        full.Add(r);
        // The only failure that matters: a real stereo photograph called 2D.
        if (r.decided && r.layout == StereoLayout::Mono) misses.push_back(f);

        DecodedImage l = LeftEye(img);
        if (l.Valid())
            mono.Add(StereoDetect::AnalyzeRGBA(l.pixels.data(), l.width, l.height,
                                               (ptrdiff_t)l.width * 4, g_params));
        DecodedImage sq = SqueezeHalves(img);
        if (sq.Valid())
            half.Add(StereoDetect::AnalyzeRGBA(sq.pixels.data(), sq.width, sq.height,
                                               (ptrdiff_t)sq.width * 4, g_params));
    }

    if (skippedNarrow)
        std::printf("  (%d file(s) skipped: aspect < 3.0, not a full-SBS source)\n\n",
                    skippedNarrow);
    full.Report("sbs-full", "full, never mono");
    mono.Report("mono", "mono");
    half.Report("sbs-half", "half, never mono");

    // Threshold grid. The two error rates pull against each other, and the policy says
    // which one to protect: a real stereo photo called 2D is the failure that hurts, and
    // an undetected mono file merely renders as SBS -- which is the default anyway.
    auto countMono = [](const Acc& a, float mpm, float svr) {
        int c = 0;
        for (const Sample& s : a.samples)
            if (!s.stereoRung && !s.abstained && s.peak < mpm && s.seam < svr) ++c;
        return c;
    };
    const float peaks[] = {0.20f, 0.25f, 0.30f, 0.40f, 0.55f};
    const float seams[] = {1.0f, 1.5f, 2.0f, 2.5f, 3.5f};
    std::printf("\n  threshold grid — SBS wrongly called 2D (full+half, MUST be 0) "
                "| mono correctly detected of %d\n", mono.n);
    std::printf("      %-10s", "monoPeak\\seam");
    for (float sv : seams) std::printf("%12.1f", (double)sv);
    std::printf("\n");
    for (float mp : peaks) {
        std::printf("      %-10.2f", (double)mp);
        for (float sv : seams) {
            const int bad = countMono(full, mp, sv) + countMono(half, mp, sv);
            const int good = countMono(mono, mp, sv);
            char cell[24];
            std::snprintf(cell, sizeof(cell), "%d|%d", bad, good);
            std::printf("%12s", cell);
        }
        std::printf("\n");
    }

    std::printf("\n  real SBS wrongly called 2D: %zu / %d\n", misses.size(), full.n);
    for (size_t i = 0; i < misses.size() && i < 12; ++i)
        std::printf("    %s\n", misses[i].c_str());
    return misses.empty() ? 0 : 1;
}

int RunPolicy(const std::string& dir, bool useContent, int& missing) {
    int failures = 0, checked = 0;
    std::printf("\n  === policy: %s ===\n",
                useContent ? "DEFAULT (filename / metadata / detector / assume-stereo)"
                           : "MEDIAPLAYER_STEREO_DETECT=meta (no pixel detector)");
    std::printf("  %-28s %-9s %-24s %6s %6s  %s\n", "asset", "layout", "signal",
                "peak", "margin", "verdict");
    std::printf("  %-28s %-9s %-24s %6s %6s  %s\n", "----------------------------",
                "---------", "------------------------", "------", "------", "-------");

    for (const Expect& e : kExpected) {
        const std::string path = (std::filesystem::path(dir) / e.file).string();
        if (!std::filesystem::exists(path)) {
            std::printf("  %-28s %-9s %-24s %6s %6s  MISSING\n", e.file, "-", "-", "-", "-");
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
        char pk[8] = "  -", mg[8] = "  -";
        if (det.peak != 0.0f || det.margin != 0.0f) {
            std::snprintf(pk, sizeof(pk), "%6.3f", (double)det.peak);
            std::snprintf(mg, sizeof(mg), "%6.3f", (double)det.margin);
        }
        std::printf("  %-28s %-9s %-24s %6s %6s  %s\n", e.file, LayoutName(got.layout),
                    SignalName(got.signal), pk, mg, ok ? "ok" : "FAIL");
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
    // --scan <dir> [limit]: measure against a real photographic corpus instead of the
    // generated fixtures.
    if (argc > 2 && std::strcmp(argv[1], "--scan") == 0) {
        if (const char* pf = std::getenv("SWEEP_PREFILTER")) {
            if (std::strcmp(pf, "none") == 0) g_params.prefilter = mp::StereoPrefilter::None;
            else if (std::strcmp(pf, "laplace") == 0)
                g_params.prefilter = mp::StereoPrefilter::Laplacian;
            else g_params.prefilter = mp::StereoPrefilter::SobelX;
            std::printf("prefilter=%s\n", pf);
        }
        if (const char* v = std::getenv("SWEEP_SEAM"))
            g_params.seamVetoRatio = (float)std::atof(v);
        if (const char* v = std::getenv("SWEEP_MONOPEAK"))
            g_params.monoPeakMax = (float)std::atof(v);
        std::printf("monoPeakMax=%.2f seamVetoRatio=%.2f\n",
                    (double)g_params.monoPeakMax, (double)g_params.seamVetoRatio);
        const int limit = argc > 3 ? std::atoi(argv[3]) : 200;
        return ScanCorpus(argv[2], limit);
    }
    if (argc > 3 && std::strcmp(argv[1], "--dumpmpo") == 0) return DumpMpo(argv[2], argv[3]);
    if (argc > 2 && std::strcmp(argv[1], "--list") == 0) return ListDir(argv[2]);
    const std::string dir = argc > 1 ? argv[1] : "assets/media/detect";
    if (!std::filesystem::is_directory(dir)) {
        std::printf("stereo_sweep: '%s' not present — run scripts/gen_stereo_test_assets.sh\n",
                    dir.c_str());
        std::printf("  (skipping; this is not a failure)\n");
        return 0;
    }
    if (const char* pf = std::getenv("SWEEP_PREFILTER")) {
        if (std::strcmp(pf, "none") == 0) g_params.prefilter = mp::StereoPrefilter::None;
        else if (std::strcmp(pf, "laplace") == 0)
            g_params.prefilter = mp::StereoPrefilter::Laplacian;
        else g_params.prefilter = mp::StereoPrefilter::SobelX;
        std::printf("stereo_sweep: prefilter=%s\n", pf);
    }
    std::printf("stereo_sweep: %s\n", dir.c_str());

    int missing = 0;
    int failures = RunPolicy(dir, /*useContent=*/true, missing);    // the shipping default
    failures += RunPolicy(dir, /*useContent=*/false, missing);      // detector disabled
    if (missing) std::printf("\n  %d asset(s) missing\n", missing);
    return failures == 0 ? 0 : 1;
}
