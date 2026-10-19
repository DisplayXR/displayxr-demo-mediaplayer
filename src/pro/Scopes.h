// SPDX-License-Identifier: Apache-2.0
//
// pro::Scopes — the monitoring scopes (the plan's "waveform, vectorscope, histogram,
// pixel probe, in 2D zones"): measured on the CPU from a small copy of the frame's left
// eye AFTER the grade, so they show what the panel shows. The grade is the shader's own
// model (exposure in linear light -> ASC CDL -> 3D LUT, shaders/sbs.frag), run per
// analysis pixel. The render thread only gathers a small point-sampled copy of the eye
// (Gather, well under a millisecond at 8K); the box filter, the grade and the bins run on
// ScopeWorker's thread. tests/scopes_test.cpp.
#pragma once

#include "color/CubeLut.h"
#include "media/FrameRing.h"
#include "media/StereoTypes.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
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

// Render-thread half: point-sample the frame's left eye into `mini`, a frame of the same
// pixel format kGatherScale x the analysis size (`w` x `h`, from `analysisW` and the
// eye's display aspect). No colour maths, so it is cheap at any source size. False for
// frames with no CPU pixels. DownsampleMini then box-filters it to w x h RGBA.
constexpr int kGatherScale = 1;
bool Gather(const FrameRing::Frame& f, StereoLayout layout, int analysisW, FrameRing::Frame& mini, int& w, int& h);
bool DownsampleMini(const FrameRing::Frame& mini, int w, int h, std::vector<uint8_t>& rgba);

// Scope pictures (RGBA, alpha 255): the waveform as `outW` x kLevels (luma, or the RGB
// parade side by side), the vectorscope as kVecSize x kVecSize. Log-scaled phosphor look.
void RenderWaveform(const Result& r, bool parade, int outW, std::vector<uint8_t>& rgba);
void RenderVectorscope(const Result& r, std::vector<uint8_t>& rgba);

// The scopes' worker: latest-wins jobs off the render thread. A job carries either a
// gathered frame (downsampled here) or a ready analysis image (a still); the worker keeps
// the last analysis image, so a grade-only job re-measures it without a new frame.
class ScopeWorker {
public:
    ScopeWorker();
    ~ScopeWorker();
    ScopeWorker(const ScopeWorker&) = delete;
    ScopeWorker& operator=(const ScopeWorker&) = delete;

    struct Job {
        bool hasFrame = false;
        FrameRing::Frame mini;            // from Gather (hasFrame)
        int w = 0, h = 0;                 // analysis size
        bool hasImage = false;
        std::vector<uint8_t> image;       // ready analysis RGBA (hasImage)
        Grade grade;                      // grade.lut points into `lut`
        std::shared_ptr<const color::CubeLut> lut;
        bool parade = false, pictures = true;  // render the waveform / vectorscope pictures
        int pictureW = 480;
    };
    void Submit(Job&& job);               // replaces a job not started yet
    // A finished measurement, if one is newer than the last Take (swapped in, no copy).
    bool Take(Result& r, std::vector<uint8_t>& wave, std::vector<uint8_t>& vec);
    double LastMs() const;                // worker time of the last measurement

private:
    void Run();
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false, pending_ = false, ready_ = false;
    Job job_;
    Result result_;
    std::vector<uint8_t> wave_, vec_;
    double lastMs_ = 0.0;
    std::vector<uint8_t> image_;          // worker-owned: the last analysis image
    int imageW_ = 0, imageH_ = 0;
    std::thread thread_;
};

}  // namespace mp::pro::scopes
