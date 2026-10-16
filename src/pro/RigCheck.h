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
//   eye sync        from the source (two inputs paired by timecode) when it has one, and
//                   MEASURED from the pixels: a short burst of consecutive frames, each
//                   eye compared against the other eye's last K frames (MeasureEyeSync)
//
// Eye swap: every measurement is of the eyes AS SHOWN. With the player's X toggle (or a
// R|L container) on, the right half of the frame is the left eye, so signs stay right.
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
    float xStretch = 1.f;         // displayed / stored eye width (2 for half-SBS)
    std::vector<float> left, right;  // eyeW * h luma (0..255)
    float meanRgb[2][3] = {};     // per eye, display-referred 0..1
};

// Build an EyePair from an NV12/I420 side-by-side frame (SbsFull or SbsHalf; both eyes
// are side by side in the frame). analysisW = the per-eye width to measure at.
// swapEyes = the frame's RIGHT half is the left eye (the effective eye swap). False for
// mono layouts or an empty frame.
bool Downsample(const FrameRing::Frame& f, StereoLayout layout, int analysisW, EyePair& out,
                bool swapEyes = false);

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
    // Measured eye sync (MeasureEyeSync): frames between the eyes, 0 = in step, -1 = not
    // measured (no burst, too little motion, or no clear winner). lateEye: +1 the right
    // eye is late, -1 the left, 0 in step / unknown.
    int eyeSyncMeasuredFrames = -1;
    int eyeSyncLateEye = 0;
};

struct Options {
    int block = 16;               // block size at the analysis resolution
    int step = 28;                // grid step
    float maxParallaxPct = 8.f;   // horizontal search range, +-% of eye width
    int maxDy = 8;                // vertical search range, +-px at the analysis resolution
    float minNcc = 0.80f;
    float minTexture = 6.f;       // block luma std below this is too flat to match
    int minMatches = 20;          // fewer inliers than this = not measured
};

Result Measure(const EyePair& e, const Options& o = {});

// Temporal eye sync from consecutive frames (oldest first, all the same size; the last is
// the current one). Compares the current right eye with the last K left eyes and the
// current left eye with the last K right eyes (K = seq.size() - 1), after removing the
// rig's mean parallax / vertical offset (shiftX / shiftY, analysis px, right minus left).
// Returns the frame offset (0 = in step), or -1 when there is too little motion or no
// clear winner; lateEye as in Result.
int MeasureEyeSync(const std::vector<EyePair>& seq, float shiftX, float shiftY, int* lateEye);

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
    int eyeOffsetFrames = -1;       // from the source's timecodes (-1 unknown)
    int eyeSyncMeasured = -1;       // measured from the pixels (-1 not measured)
    int eyeSyncLateEye = 0;         // +1 right late, -1 left late
};

struct Alert {
    double start = 0.0, end = 0.0;  // seconds
    std::string what;               // "Foreground at -2.1% for 3 s"
    std::string timecode;           // source timecode when it started
    std::string endTimecode;        // source timecode when it ended (last good one after a cut)
    std::string wallClock;          // HH:MM:SS when it started
};

// The measurement thread + the 60-second history + budget alerts.
class RigMonitor {
public:
    RigMonitor();
    ~RigMonitor();
    // Render thread: hand over every frame. About 4 times a second a burst of kSyncFrames
    // consecutive frames is downsampled (~2 ms each) and measured on the worker; the rest
    // return at once. swapEyes = the effective eye swap (the frame's right half is shown to
    // the left eye).
    static constexpr int kSyncFrames = 4;  // current + K = 3 earlier frames
    void Submit(const FrameRing::Frame& f, StereoLayout layout, int eyeOffsetFrames,
                const std::string& timecode, bool swapEyes = false);
    // Record one measurement (the worker's path; public so tests can drive the history and
    // the alerts with their own clock).
    void AddSample(const Sample& s, const std::string& timecode);
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
    void AddSampleLocked(const Sample& s, const std::string& timecode);  // under m_
    void UpdateAlerts(const Sample& s, const std::string& timecode);

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;
    bool pending_ = false;
    std::vector<EyePair> work_;      // the burst, oldest first; the last is measured
    // burst being collected (render thread only)
    std::vector<EyePair> burst_;
    int64_t burstLastSerial_ = 0;
    double burstStart_ = -1.0;
    bool burstSwap_ = false;
    StereoLayout burstLayout_ = StereoLayout::Mono;
    int pendingEyeOffset_ = -1;
    std::string pendingTimecode_;
    double lastSubmit_ = 0.0;
    Result last_;
    std::deque<Sample> history_;
    std::vector<Alert> alerts_;
    Budget budget_;
    double measureMs_ = 0.0;
    uint64_t generation_ = 0;  // bumped by Reset: a measurement started before it is dropped
    // open alert runs: near, far, vertical, colour
    struct Run {
        double since = -1;
        float worst = 0;
        std::string tc, clock;
    };
    Run runs_[4];
    void CloseRun(int i, double end, const std::string& endTc);
    // the last measured sample, for closing runs when the picture stops measuring
    double lastGoodT_ = -1.0, notOkSince_ = -1.0;
    std::string lastGoodTc_;
};

}  // namespace mp::rig
