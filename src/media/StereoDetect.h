// SPDX-License-Identifier: Apache-2.0
//
// StereoDetect — decide whether a frame's two halves are a stereo pair, by pixels (#45).
//
// This is the layer that solves the case nothing else can. A half-SBS frame is
// dimensionally identical to a mono frame, so the filename and the aspect ratio simply
// do not carry the answer; the pixels do. Two halves of a genuine side-by-side frame are
// near-copies of each other separated by a small horizontal disparity, which shows up as
// a HIGH AND SHARP normalised-cross-correlation peak at a SMALL shift. Mono content has
// no such peak.
//
// All three conditions are required. A high score alone is not enough: repetitive
// content (brick, fences, railings, a two-panel collage) correlates well at many shifts,
// so the peak is compared against a baseline measured at far-away shifts and rejected
// unless it stands out. Mirror-symmetric content (a centred building) is separately safe
// because mirror symmetry produces no SHIFT peak.
//
// Deliberately dependency-light: this header pulls in only StereoTypes.h, and the .cpp
// adds nothing but Log.h and the C++ standard library. No SDL, no Vulkan, no libav --
// which is what lets the whole thing be unit-tested against synthetic buffers.
#pragma once

#include "StereoTypes.h"

#include <cstddef>
#include <cstdint>

namespace mp {

class StereoDetect {
public:
    // Analyse an 8-bit luma plane. `rowStride` is in bytes; `pixelStride` is in bytes
    // between horizontally adjacent samples -- pass 2 (with `y` advanced by one byte) to
    // sample the high byte of a 10-bit little-endian plane, which is otherwise read as
    // garbage that silently abstains.
    static StereoDetectResult AnalyzeLuma(const uint8_t* y, int w, int h,
                                          ptrdiff_t rowStride, int pixelStride = 1,
                                          const StereoDetectParams& p = StereoDetectParams{});

    // Analyse RGBA8 (what stb hands us). Builds luma internally, Rec.601 weights.
    static StereoDetectResult AnalyzeRGBA(const uint8_t* rgba, int w, int h,
                                          ptrdiff_t rowStride,
                                          const StereoDetectParams& p = StereoDetectParams{});

    // Full-vs-half, decided ONLY after a frame is known to be SBS. Full SBS packs two
    // full-width eyes, so the frame aspect is ~2x the per-eye aspect; half SBS squeezes
    // each eye, so the frame aspect ~equals the per-eye display aspect. Whichever of
    // `a` and `a/2` sits closer to a plausible single-eye aspect wins.
    // `ambiguous` is set when the two are too close to call.
    static StereoLayout ChooseFullOrHalf(float activeAspect, bool& ambiguous);
};

// Majority vote across video frames. Abstained samples are ignored rather than counted
// as mono -- which is exactly what stops a fade-from-black opening from deciding the
// layout of the whole clip.
class StereoVote {
public:
    void Add(const StereoDetectResult& r);
    int Samples() const { return samples_; }
    bool Sufficient() const { return samples_ >= kVoteMin; }
    // Majority layout; ties broken by higher mean confidence. `decided` is false until
    // Sufficient() (or AcceptSingle() was used for a stream too short to reach it).
    StereoDetectResult Result() const;
    // A stream that yields fewer than kVoteMin frames in total would otherwise never
    // decide. Call this once the source is exhausted to accept what we have.
    void AcceptSingle() { acceptSingle_ = true; }
    void Reset();

    static constexpr int kVoteMin = 3;   // samples needed before we trust the vote
    static constexpr int kVoteWant = 7;  // samples after which we stop early

private:
    int samples_ = 0;
    int count_[3] = {0, 0, 0};           // indexed by StereoLayout
    float conf_[3] = {0.0f, 0.0f, 0.0f}; // summed confidence per layout
    StereoDetectResult best_{};          // highest-confidence sample, for its stats
    bool acceptSingle_ = false;
};

} // namespace mp
