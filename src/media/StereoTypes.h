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
struct StereoDetectParams {
    int   gridWidth        = 96;    // per-half analysis grid width, in cells
    float peakMin          = 0.88f; // min NCC at the best shift to call it stereo
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
    float monoPeakMax      = 0.70f; // below this -> CONFIDENT mono (beats the aspect rule)
    float monoMarginMax    = 0.03f; // ...or a margin this flat
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
    int   cropX0 = 0, cropY0 = 0, cropX1 = 0, cropY1 = 0;  // active picture area
    const char* reason = "";   // static string; never owns storage
};

} // namespace mp
