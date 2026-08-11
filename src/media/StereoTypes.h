// SPDX-License-Identifier: Apache-2.0
//
// StereoTypes — the vocabulary shared by everything that has an opinion about how a
// frame is packed (#45).
//
// `StereoLayout` lives here rather than in MediaSource.h because the layered detector
// works the other way round: MediaSource composes a verdict out of a detector result,
// and the detector needs the layout enum. Putting the enum in the leaf header keeps
// that dependency acyclic, and keeps StereoDetect free of MediaSource entirely.
//
// This header deliberately depends on nothing but <cstdint>, so the pure detector can
// be unit-tested without dragging in SDL, Vulkan, or libav.
#pragma once

#include <cstdint>

namespace mp {

enum class StereoLayout {
    Mono,     // 2D — same image to both eyes
    SbsFull,  // full side-by-side: each eye is half the pixel width
    SbsHalf,  // half side-by-side: each eye is squeezed; stretched on display
};

// Which layer of the layered detector actually decided the layout. Ordered loosely by
// authority, but the resolution order is spelled out in MediaSource::Resolve() — do not
// infer priority from the enum values.
enum class StereoSignal : uint8_t {
    Default,   // nothing had an opinion -> mono
    Manual,    // the user pinned it (L key / HUD button) — beats everything
    Metadata,  // container said so: AVStereo3D side data, or a LIF / MPO container
    Filename,  // the `*_2x1` / `*_half_2x1` naming convention
    Content,   // the cross-correlation detector
    Aspect,    // frame aspect alone — weak, last resort
};

const char* LayoutName(StereoLayout l);
const char* SignalName(StereoSignal s);

// Tunables for StereoDetect, gathered so tests can sweep them without touching code.
// Defaults are the shipping values; see StereoDetect.cpp for the derivation of each.
// High-pass applied to the analysis grid before correlating. Raw intensity lets
// LOW-FREQUENCY content — vignetting, a sky gradient, a lighting ramp — correlate
// between two UNRELATED halves, which lifts the mono floor and eats into the gap that
// separates mono from stereo. A high-pass makes the match illumination-invariant, which
// is why practical stereo matchers work on gradients rather than raw pixels.
enum class StereoPrefilter { None, SobelX, Laplacian };

struct StereoDetectParams {
    StereoPrefilter prefilter = StereoPrefilter::SobelX;
    int   gridWidth        = 96;    // per-half analysis grid width, in cells
    // Min NCC at the best shift to call it stereo POSITIVELY. Recalibrated for the
    // SobelX high-pass, which strips the smooth low-frequency component that used to
    // inflate every score: with it, real stereo pairs span 0.08-0.99 (mean 0.64) rather
    // than clustering near 1.0. 0.88 left only ~11% of a real corpus positively
    // identified. 0.55 is safe because it is more than twice the highest peak any real
    // MONO frame reached (0.233), and the margin and disparity gates still apply.
    //
    // Note this rung is not load-bearing for correctness: a stereo file that fails it
    // falls through to the stereo assumption and renders correctly anyway. It matters for
    // full-vs-half on letterboxed content, and for pre-empting the mono rung.
    float peakMin          = 0.55f;
    float marginMin        = 0.10f; // min (peak - far baseline): peak must be SHARP
    float maxDisparityFrac = 0.06f; // max |best shift|, as a fraction of half-width
    float searchFrac       = 0.12f; // shift search half-range, fraction of half-width
    float minSigma         = 8.0f;  // min per-half std-dev (0..255) or we abstain
    // Min mean|dI/dx| as a FRACTION of sigma. Relative, not absolute: box-averaging down
    // to a ~96-cell grid strips most of an image's gradient energy, so an absolute floor
    // calibrated on full-resolution intuition rejects ordinary content. (Measured: a
    // high-contrast test plate with sigma 59 grades out at |dI/dx| 1.79 -- an absolute
    // threshold of 2.0 abstained on the entire corpus.) What this gate is really for is
    // horizontally-UNIFORM content, where a horizontal shift search is meaningless; that
    // shows up as a gradient near zero relative to the variation present.
    float minGradRatio     = 0.01f;
    int   barLevel         = 16;    // luma <= this counts as a letterbox-bar sample
    float barFrac          = 0.02f; // a row/col is a bar if < this fraction exceeds barLevel
    float maxCropFrac      = 0.40f; // never crop more than this off an axis
    // --- the mono gate -------------------------------------------------------------
    //
    // Both numbers below were measured on 2380 real 7680x2160 stereo photographs plus the
    // two variants each one yields for free (its left eye alone = real mono; its halves
    // squeezed = real half-SBS) -- 7140 analyses, NOT synthetic fixtures. That matters:
    // synthetic mono peaks at 0.05, but real mono, with its sky gradients, defocus and
    // vignetting, reaches 0.361; and real SBS falls all the way to 0.000, because a
    // stereo pair is a depth-dependent warp rather than one global shift. The two
    // populations OVERLAP, so no threshold separates them cleanly and the choice is
    // purely about which way to be wrong.
    //
    // Grid over that corpus (SBS wrongly called 2D | mono correctly detected, of 2380):
    //     monoPeak\seam    1.0        2.0        2.5        3.5
    //         0.30      0|290     0|2318     0|2357     0|2367
    //         0.40      0|290     0|2319     0|2358     3|2368
    //         0.55      3|290     6|2319     9|2358    20|2368
    //
    // Below this peak the halves do not match at any plausible shift. 0.30 is the loosest
    // value that still gives ZERO false-mono across the whole corpus; 0.55 gives 20.
    // Erring low is the safe direction: an undetected mono file just renders as SBS,
    // which is the default anyway, whereas calling a real stereo photograph 2D is the
    // failure that actually hurts.
    float monoPeakMax      = 0.30f;
    // How many times its neighbourhood the mid-frame seam must exceed to VETO a mono
    // verdict. Real mono has a median seam of 1.32, real SBS a median of 25.6, and for
    // the low-peak SBS frames that are the actual risk the 5th percentile is still 7.5 --
    // a stereo pair with a large depth spread scores LOW on correlation but HIGH on the
    // seam, so the two signals are anti-correlated exactly where it helps. 3.5 keeps
    // 2367/2380 mono detections; dropping to 1.0 would cost all but 290 of them.
    float seamVetoRatio    = 3.5f;
};

// What the content detector concluded, plus everything needed to explain it in a log.
struct StereoDetectResult {
    bool  decided   = false;   // trust `layout` (which may legitimately be Mono)
    bool  abstained = false;   // degenerate input: the detector says nothing at all
    StereoLayout layout = StereoLayout::Mono;
    float confidence = 0.0f;   // 0..1, only meaningful when decided
    float peak = 0.0f;         // best NCC over the shift search
    float baseline = 0.0f;     // high percentile of NCC at implausibly-far shifts
    float margin = 0.0f;       // peak - baseline: how SHARP the peak is
    int   disparityPx = 0;     // best shift, in grid cells, signed
    float sigmaMin = 0.0f;     // min per-half std-dev
    float gradMin = 0.0f;      // min per-half mean |dI/dx|
    float seam = 0.0f;         // mid-frame discontinuity, relative to its neighbourhood
    int   cropX0 = 0, cropY0 = 0, cropX1 = 0, cropY1 = 0;  // active picture area
    const char* reason = "";   // static string; never owns storage
};

} // namespace mp
