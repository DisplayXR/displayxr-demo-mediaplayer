// SPDX-License-Identifier: Apache-2.0
//
// pro::Scopes — the monitoring scopes (the plan's "waveform, vectorscope, histogram,
// pixel probe, in 2D zones"): measured on the CPU from a small copy of the frame's left
// eye AFTER the grade, so they show what the panel shows. The grade is the shader's own
// model (exposure in linear light -> ASC CDL -> 3D LUT, shaders/sbs.frag), run per
// analysis pixel. tests/scopes_test.cpp.
#pragma once

#include "color/CubeLut.h"
#include "media/FrameRing.h"
#include "media/StereoTypes.h"

#include <cstdint>
#include <vector>

namespace mp::pro::scopes {

// The grade as the shader applies it. `lut` may be null (no LUT).
struct Grade {
    float exposure = 0.0f;
    bool cdlOn = false;
    float slope[3] = {1.f, 1.f, 1.f}, offset[3] = {0.f, 0.f, 0.f}, power[3] = {1.f, 1.f, 1.f};
    float saturation = 1.0f;
    const color::CubeLut* lut = nullptr;  // used when non-null and Valid()
};

// One display-referred R'G'B' (0..1) through the grade, exactly as Grade() in sbs.frag.
void ApplyGrade(const Grade& g, float rgb[3]);

constexpr int kLevels = 256;   // waveform / histogram vertical resolution
constexpr int kVecSize = 128;  // vectorscope image is kVecSize x kVecSize

struct Result {
    bool ok = false;
    int w = 0, h = 0;                  // analysis image (the left eye, display aspect)
    std::vector<uint8_t> graded;       // w * h * 3, graded R'G'B' bytes (the pixel probe reads it)
    // Waveform: for each analysis column x, counts per level, luma and R, G, B.
    std::vector<uint32_t> waveY, waveR, waveG, waveB;  // w * kLevels each, [x * kLevels + level]
    std::vector<uint32_t> vector;      // kVecSize^2 counts of (Cb, Cr), BT.709, centre = neutral
    uint32_t histR[kLevels] = {}, histG[kLevels] = {}, histB[kLevels] = {}, histY[kLevels] = {};
    float minY = 0, maxY = 0, meanY = 0;  // graded luma, 0..1
    double ms = 0.0;                   // measurement time
};

// The analysis image: the frame's left eye box-filtered to `analysisW` wide (the height
// follows the eye's display aspect), UNGRADED RGBA. Kept by the caller, so a paused frame
// or a still is re-measured when the grade changes. False when the frame has no CPU pixels.
bool Downsample(const FrameRing::Frame& f, StereoLayout layout, int analysisW, std::vector<uint8_t>& rgba,
                int& w, int& h);
// The same from an RGBA still (the whole image, both eyes when SBS).
bool DownsampleRgba(const uint8_t* src, int srcW, int srcH, StereoLayout layout, int analysisW,
                    std::vector<uint8_t>& rgba, int& w, int& h);
// Grade the analysis image and fill the scopes.
void Measure(const std::vector<uint8_t>& rgba, int w, int h, const Grade& g, Result& out);

// Scope pictures (RGBA, alpha 255): the waveform as `outW` x kLevels (luma, or the RGB
// parade side by side), the vectorscope as kVecSize x kVecSize. Log-scaled phosphor look.
void RenderWaveform(const Result& r, bool parade, int outW, std::vector<uint8_t>& rgba);
void RenderVectorscope(const Result& r, std::vector<uint8_t>& rgba);

}  // namespace mp::pro::scopes
