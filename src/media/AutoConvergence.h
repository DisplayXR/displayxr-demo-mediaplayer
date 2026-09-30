// SPDX-License-Identifier: Apache-2.0
//
// AutoConvergence (#92) — measure the disparity of the subject of a live stereo feed and
// turn it into a smoothed, comfort-clamped convergence shift that puts that subject at
// zero parallax (the display plane).
//
// The measurement is a faithful port of displayxr-web's `js/call/disparity.js` (the 3D
// video call, RFC 0002 §3): NCC block matching on a downsampled luma plane, a uniqueness
// test against periodic textures, the nearest strong disparity mode as the subject, a
// template tracker with a full re-search every 2 s, and a median-of-3 hold. The numbers
// (MIN_NCC 0.8, UNIQUENESS 0.08, block = E/20, grid step 1.5 x block, ROI x 15-85 % /
// y 5-85 %, d in [-5 %, +30 %] of eye width, +-2 rows, nearest-mode tol 2 px / 12 % /
// 3 blocks) are kept so web and native converge the same way. It MEASURES: no face
// detection, no calibration, no rectification.
//
// What differs from the web: the subject selector is pluggable (SubjectPolicy), and the
// policy layer (AutoConvergenceController) adds a comfort clamp and a slow rate limit
// with a scene-cut snap on top of the web's low-pass.
//
// SIGN / UNITS. `d` (and Result::disparityFrac) follow disparity.js: d = x_left - x_right
// of the same scene point, positive = crossed = in front of the display plane, expressed
// in downsampled pixels (d) or as a fraction of the eye width (disparityFrac). The app's
// convergence is a per-eye shift as a fraction of the eye's width, applied +shift to the
// left view and -shift to the right (ConvergenceShiftPx below, the exact expression the
// draw code uses). A point with disparity d is displayed with disparity d + 2*shift, so
// the subject lands at zero parallax for shift = -d/2 — the same sign as the legacy
// StereoCompose::EstimateAutoConvergence (which returns 0.5 * (x_right - x_left)).
//
// Pure: no SDL, no Vulkan, no App. The worker thread is std::thread only.
#pragma once

#include "media/StereoTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp::autoconv {

// ---- disparity.js constants -------------------------------------------------------------
constexpr float kMinTexture = 6.0f;   // block luma std below this is too flat to match
constexpr float kMinNcc = 0.8f;       // a block match must reach this NCC
constexpr float kUniqueness = 0.08f;  // ...and beat its best rival peak (>= 3 px away) by this
constexpr int kMaxEyeWidth = 240;     // downsample target: <= 240 px per eye

// The draw code's per-eye convergence shift, in pixels: +shift for the left view, -shift
// for the right. App.cpp calls this, so the sign test in tests/ exercises the real math.
inline int32_t ConvergenceShiftPx(float shiftFrac, uint32_t widthPx, bool isLeftView) {
    return (int32_t)(shiftFrac * (float)widthPx * (isLeftView ? 1.0f : -1.0f));
}

// A grayscale side-by-side image: left eye in columns [0, W/2), right eye in [W/2, W).
struct SbsGray {
    const uint8_t* p = nullptr;
    int W = 0;  // full width (both eyes), even
    int H = 0;
    int E() const { return W / 2; }
};

struct Match {
    float d = 0.f;  // x_left - x_right, sub-pixel
    int dy = 0;     // best row offset of the right-eye block
    float c = -2.f; // NCC at the best position
};

struct Block {
    int x = 0, y = 0;  // top-left in LEFT-eye pixels
    float d = 0.f;
    int dy = 0;
    float c = 0.f;
    float sharp = 0.f;  // mean |dI/dx| + |dI/dy| over the block (SubjectPolicy::Sharpness)
};

// Search options; sentinel values mean "derive from the eye width as disparity.js does".
struct SearchOptions {
    int block = 0;           // 0 -> max(8, round(E/20))
    int step = 0;            // 0 -> round(block * 1.5)
    int dMin = kAutoRange;   // kAutoRange -> -round(E * 0.05)
    int dMax = kAutoRange;   // kAutoRange -> round(E * 0.30)
    float roiX0 = 0.15f, roiX1 = 0.85f, roiY0 = 0.05f, roiY1 = 0.85f;
    int dyMax = 2;
    float uniq = kUniqueness;
    float minNcc = kMinNcc;
    float tol = 2.f;         // nearest mode: bin half-width (px)
    float minFrac = 0.12f;   //   ...share of matched blocks the mode needs
    int minBlocks = 3;       //   ...and at least this many
    float refineW = 3.f, refineH = 1.5f;  // focus refine window, in blocks
    static constexpr int kAutoRange = -1000000;
};

// Best horizontal disparity of one left-eye block, searched over [dMin, dMax] and +-dyMax
// rows, coarse-to-fine, with the uniqueness test (wide searches only) and a sub-pixel
// parabola. False when the block is flat or no candidate was valid.
bool MatchBlock(const SbsGray& img, int x0, int y0, int bw, int bh, int dMin, int dMax,
                int dyMax, float uniq, Match& out);

// Block disparities over the ROI of the left eye (only confident matches, c >= minNcc).
std::vector<Block> BlockDisparities(const SbsGray& img, const SearchOptions& o = {});

// The nearest strong disparity mode: largest d whose +-tol bin holds >= max(minBlocks,
// ceil(n * minFrac)) blocks. Returns false if none; `d` = the members' median.
bool NearestMode(const std::vector<Block>& blocks, float tol, float minFrac, int minBlocks,
                 float& d, std::vector<Block>* members = nullptr);

// Where the eyes are in a subject blob (centre of its top rows, ~40 % down a head).
void EyeMidpoint(const std::vector<Block>& members, int block, float& x, float& y);

enum class Method { Mode, Focus, Track, Sharp, Centre };
const char* MethodName(Method m);

struct Measurement {
    bool ok = false;
    float d = 0.f;        // downsampled px
    float x = 0.f, y = 0.f;  // focus point, left-eye px
    float c = 0.f;
    Method method = Method::Mode;
    int blocks = 0;
};

// disparity.js measureFocusDisparity. `focus` (left-eye px) skips the subject search.
// When a block search ran, its confident blocks are copied to `blocksOut`.
Measurement MeasureFocusDisparity(const SbsGray& img, const SearchOptions& o,
                                  const float* focusXY, std::vector<Block>* blocksOut);

// disparity.js createDisparityTrack: median of the last n accepted values; holds the last
// good value through failed measurements.
class DisparityTrack {
public:
    explicit DisparityTrack(int n = 3) : n_(n) {}
    // Push a measurement (ok=false = dropout). Returns whether a value exists.
    bool Push(bool ok, float d);
    bool HasValue() const { return has_; }
    float Value() const { return value_; }
    void Reset() { hist_.clear(); has_ = false; value_ = 0.f; }

private:
    int n_;
    std::vector<float> hist_;
    bool has_ = false;
    float value_ = 0.f;
};

// disparity.js createFocusTracker: a full subject search locks on; after that a left-eye
// template is re-found near its last position and matched in a +-dWin disparity window.
// The full search re-runs every fullEveryMs (with a jump guard against aliasing).
class FocusTracker {
public:
    struct Config {
        double fullEveryMs = 2000.0;
        int dWin = 4, rx = 12, ry = 8;
        float minTrackNcc = 0.75f;
        float jump = 8.f;
    };
    FocusTracker() = default;
    explicit FocusTracker(const Config& c) : cfg_(c) {}
    Measurement Measure(const SbsGray& img, double nowMs, const SearchOptions& o,
                        const float* focusXY, std::vector<Block>* blocksOut);
    void Reset() { st_.valid = false; pendingValid_ = false; }

private:
    struct State {
        bool valid = false;
        int x = 0, y = 0, w = 0, h = 0;
        float d = 0.f;
        double at = 0.0;
        std::vector<float> tpl;
    };
    Config cfg_{};
    State st_{};
    bool pendingValid_ = false;
    float pendingD_ = 0.f;
};

// disparity.js downsampleLuma: integer-factor box (f <= 2) or 2x2 quarter-point sample
// (f > 2) of a strided plane; the output width is even. With `swapEyes` the two halves
// are exchanged row by row, so the output's LEFT half is the picture the viewer's left
// eye actually sees.
void DownsampleLuma(const uint8_t* src, int stride, int srcW, int srcH, int factor,
                    bool swapEyes, std::vector<uint8_t>& out, int& outW, int& outH);
// The factor that brings a per-eye width to <= kMaxEyeWidth.
int DownsampleFactorFor(int frameWidth);

// ---- the estimator ----------------------------------------------------------------------

enum class SubjectPolicy {
    Nearest,    // holocall: the nearest strong mode (a person at the camera)
    Sharpness,  // confidence-weighted median of the sharpest quartile (shallow DoF marks it)
    Centre,     // centre-Gaussian-weighted median (fallback)
    Focus,      // a user-pinned point, template-tracked
};
const char* PolicyName(SubjectPolicy p);
// "nearest" | "sharp"/"sharpness" | "centre"/"center" | "focus". False if unknown.
bool ParsePolicy(const std::string& s, SubjectPolicy& out);

struct Result {
    bool ok = false;           // THIS measurement found the subject
    bool hasValue = false;     // the median-of-3 track holds a value (fresh or held)
    float disparityFrac = 0.f; // held median d / eye width (x_left - x_right; + = in front)
    float rawFrac = 0.f;       // this measurement's d / eye width (valid when ok)
    float ncc = 0.f;
    int blocksMatched = 0;     // confident blocks of the latest block search
    bool hasRange = false;     // nearFrac/farFrac valid
    float nearFrac = 0.f;      // 95th percentile of confident block d (the nearest content)
    float farFrac = 0.f;       // 5th percentile (the farthest content)
    const char* rule = "none"; // mode | track | focus | sharp | centre | sharp>centre | none | mono
    double ms = 0.0;           // measurement time (this call)
    float focusU = 0.f, focusV = 0.f;  // subject point, left-eye normalised
    float sceneMad = 0.f;      // mean |dI| vs the previous downsampled frame
    bool sceneCut = false;     // sceneMad above threshold: tracker + hold were reset
    int eyeW = 0, eyeH = 0;    // downsampled eye size used
};

class DisparityEstimator {
public:
    static constexpr float kSceneCutMad = 20.f;

    // Measure a full-resolution luma plane. `layout` must be SbsFull or SbsHalf (the eye is
    // half the frame width either way; the half-SBS squeeze does not change a horizontal
    // disparity expressed as a fraction of eye width). `focusUV` = left-eye normalised.
    Result Measure(const uint8_t* y, int stride, int w, int h, StereoLayout layout,
                   SubjectPolicy policy, const float* focusUV, double nowMs,
                   bool swapEyes = false);
    // Measure an already-downsampled SBS gray image (what the worker does).
    Result MeasureGray(const uint8_t* g, int W, int H, SubjectPolicy policy,
                       const float* focusUV, double nowMs);
    void Reset();

private:
    FocusTracker tracker_;
    DisparityTrack track_{3};
    std::vector<uint8_t> small_, prev_;
    int prevW_ = 0, prevH_ = 0;
    SubjectPolicy lastPolicy_ = SubjectPolicy::Nearest;
    float lastFocus_[2] = {-1.f, -1.f};
    bool hasRange_ = false;
    float nearFrac_ = 0.f, farFrac_ = 0.f;
    int lastBlocks_ = 0;
    std::vector<Block> blocks_;
};

// ---- the policy layer -------------------------------------------------------------------

class AutoConvergenceController {
public:
    struct Config {
        float frontBudget = 0.005f;  // max crossed (in front) disparity, fraction of eye width
        float rearBudget = 0.015f;   // max uncrossed (behind) disparity
        float alpha = 0.2f;          // low-pass per update (web: CONVERGENCE_ALPHA)
        float updateHz = 5.f;        // the rate alpha is defined at
        float ratePerSec = 0.002f;   // max |d shift| / s, fraction of eye width
        float maxShift = 0.12f;      // web: CONVERGENCE_MAX_FRACTION
        // OFF by default: on a real scene (desk at ~20 % disparity, subject at ~2 %) a
        // 0.5 % front budget pins the desk at the plane and pushes the subject ~17 %
        // behind it. Subject-at-the-plane (the 3D call's behaviour) is the default; the
        // clamp stays as an opt-in experiment (MEDIAPLAYER_AUTO_CONV_CLAMP=1).
        bool comfortClamp = false;
    };
    AutoConvergenceController() = default;
    explicit AutoConvergenceController(const Config& c) : cfg_(c) {}
    Config& MutableConfig() { return cfg_; }

    // Target per-eye shift for a measurement (pure; exposed for tests). Writes the clamp
    // that fired ("", "front", "rear") into `clamp`.
    float TargetFor(const Result& r, const char** clamp) const;

    // A new measurement (~5 Hz). Dropouts keep the previous target (the estimator's
    // median-of-3 already holds). A scene cut or the first value skips the rate limit
    // until the value has caught up once.
    void Update(const Result& r);
    // Advance the applied value by dt seconds (render rate). Disabled -> glides to 0
    // (low-pass only, no rate limit) and lands on exactly 0.
    void Tick(double dt, bool enabled);
    float Value() const { return value_; }
    float Target() const { return target_; }
    const char* Clamp() const { return clamp_; }
    bool HasTarget() const { return hasTarget_; }
    void Reset();

private:
    Config cfg_{};
    float value_ = 0.f;
    float target_ = 0.f;
    bool hasTarget_ = false;
    bool fast_ = false;  // rate limit skipped until caught up (first lock / scene cut / off)
    bool wasEnabled_ = false;
    const char* clamp_ = "";
};

// ---- the side thread --------------------------------------------------------------------

// Owns a DisparityEstimator on a worker thread. Submit() downsamples on the CALLER's thread
// (a few hundred microseconds at 4K: ~65k sampled pixels) into a mailbox and wakes the
// worker; a frame submitted while the worker is still busy replaces the pending one.
class AutoConvergenceWorker {
public:
    ~AutoConvergenceWorker() { Stop(); }
    void Start();
    void Stop();
    bool Running() const { return thread_.joinable(); }
    // Returns the downsample time in ms.
    double Submit(const uint8_t* y, int stride, int w, int h, bool swapEyes,
                  SubjectPolicy policy, const float* focusUV, double nowMs);
    bool TakeResult(Result& out);
    void RequestReset();

private:
    void Main();
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool pending_ = false;
    bool resetReq_ = false;
    std::vector<uint8_t> staging_, work_;
    int sw_ = 0, sh_ = 0;
    SubjectPolicy policy_ = SubjectPolicy::Nearest;
    bool hasFocus_ = false;
    float focus_[2] = {0.f, 0.f};
    double nowMs_ = 0.0;
    bool hasResult_ = false;
    Result result_{};
    DisparityEstimator est_;  // worker-owned
};

}  // namespace mp::autoconv
