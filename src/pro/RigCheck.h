// SPDX-License-Identifier: Apache-2.0
//
// RigCheck — live checks of a stereo rig's picture (Media Player Pro, phase 2):
//
//   vertical error  how far the right eye sits above/below the left, in full-resolution
//                   pixels (median of sub-pixel 2D block matches; a rig tech wants < 1)
//   depth range     the near and far screen parallax of the scene, as % of the eye width,
//                   stereographer convention: parallax = x_right - x_left, NEGATIVE = in
//                   front of the screen (crossed), positive = behind
//   colour match    CIE76 delta-E between the two eyes' mean colours (the same scene
//                   through two cameras should match; > ~2 is visible)
//   eye sync        from the source (two inputs paired by timecode); not measured here
//
// Measured on a downsampled luma copy of the frame on a worker thread a few times a
// second; the 60-second history and the depth-budget alerts live in RigMonitor. Pure:
// no SDL, no Vulkan, no App. Tested against the test signal (tests/rig_check_test.cpp).
#pragma once

#include "media/FrameRing.h"
#include "media/StereoTypes.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp::rig {

// The measurement image: both eyes as grey + mean colour, at the analysis resolution.
struct EyePair {
    int eyeW = 0, h = 0;          // analysis size per eye
    float fullEyeW = 0, fullH = 0;  // the source's per-eye size (to report full-res px)
    std::vector<float> left, right;  // eyeW * h luma (0..255)
    float meanRgb[2][3] = {};     // per eye, display-referred 0..1
};

// Build an EyePair from an NV12/I420 side-by-side frame (SbsFull or SbsHalf; both eyes
// are side by side in the frame). analysisW = the per-eye width to measure at. False for
// mono layouts or an empty frame.
bool Downsample(const FrameRing::Frame& f, StereoLayout layout, int analysisW, EyePair& out);

struct Result {
    bool ok = false;
    int matches = 0;              // confident block matches used
    float verticalPx = 0.f;       // right minus left, full-res px (+ = right eye lower)
    float rollDeg = 0.f;          // vertical error slope across the width, degrees
    float nearPct = 0.f;          // 2nd percentile parallax, % of eye width (negative = in front)
    float farPct = 0.f;           // 98th percentile parallax
    float deltaE = 0.f;           // CIE76 between the eyes' mean colours
    std::vector<float> parallaxPct;  // every match's parallax (for the depth overlay)
    std::vector<float> matchX, matchY;  // match centres, 0..1 of the left eye
};

struct Options {
    int block = 16;               // block size at the analysis resolution
    int step = 28;                // grid step
    float maxParallaxPct = 8.f;   // horizontal search range, +-% of eye width
    int maxDy = 8;                // vertical search range, +-px at the analysis resolution
    float minNcc = 0.80f;
    float minTexture = 6.f;       // block luma std below this is too flat to match
};

Result Measure(const EyePair& e, const Options& o = {});

// CIE76 delta-E of two display-referred sRGB colours (0..1).
float DeltaE76(const float a[3], const float b[3]);

// ---------------------------------------------------------------------------------------

struct Budget {
    float nearPct = -2.0f;  // nearest allowed parallax (in front), % of eye width
    float farPct = 1.0f;    // farthest allowed (behind)
    float maxVerticalPx = 1.0f;
    float maxDeltaE = 2.0f;
};

struct Sample {
    double t = 0.0;  // seconds, LiveProducer::NowSeconds
    bool ok = false;
    float nearPct = 0, farPct = 0, verticalPx = 0, deltaE = 0;
    int eyeOffsetFrames = -1;
};

struct Alert {
    double start = 0.0, end = 0.0;  // seconds
    std::string what;               // "Foreground at -2.1% for 3 s"
    std::string timecode;           // source timecode when it started
    std::string wallClock;          // HH:MM:SS when it started
};

// The measurement thread + the 60-second history + budget alerts.
class RigMonitor {
public:
    RigMonitor();
    ~RigMonitor();
    // Render thread: hand over a frame (downsampled here, ~2 ms). Drops it while busy.
    void Submit(const FrameRing::Frame& f, StereoLayout layout, int eyeOffsetFrames,
                const std::string& timecode);
    void SetBudget(const Budget& b);
    Budget GetBudget() const;
    void Reset();
    // Snapshot for the UI / tools.
    Result Last() const;
    std::vector<Sample> History(double seconds = 60.0) const;
    std::vector<Alert> Alerts() const;  // most recent last, at most 20
    bool Active(double now) const;      // measured within the last 2 s
    double LastMeasureMs() const;

private:
    void Worker();
    void UpdateAlerts(const Sample& s, const std::string& timecode);

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;
    bool pending_ = false;
    EyePair work_;
    int pendingEyeOffset_ = -1;
    std::string pendingTimecode_;
    double lastSubmit_ = 0.0;
    Result last_;
    std::deque<Sample> history_;
    std::vector<Alert> alerts_;
    Budget budget_;
    double measureMs_ = 0.0;
    // open alert runs
    double nearSince_ = -1, farSince_ = -1, vertSince_ = -1, colSince_ = -1;
    float nearWorst_ = 0, farWorst_ = 0, vertWorst_ = 0, colWorst_ = 0;
    std::string nearTc_, farTc_, vertTc_, colTc_, nearClock_, farClock_, vertClock_, colClock_;
};

}  // namespace mp::rig
