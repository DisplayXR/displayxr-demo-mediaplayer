// SPDX-License-Identifier: Apache-2.0
//
// TestSignalSource — a synthetic stereo "rig" for testing live monitoring without a
// camera or capture card: moving discs at known depths in front of a gradient, both
// eyes composed side by side (SBS-full) exactly as a two-input SDI pair arrives, with
// a running SMPTE timecode. Faults a real rig has can be dialled in, so the rig checks
// have something to find:
//   vShiftPx     right eye moved down by this many pixels (vertical misalignment)
//   eyeDelay     right eye shows the scene this many frames late (eye sync)
//   gainR        right eye brightness x this (colour match; photo mode: red channel)
//   depthScale   scene disparities x this (photo mode: extra crossed parallax)
//   rollDeg      right eye rotated about its centre by this many degrees (+ = clockwise
//                on screen: its right side lower), the rig check's roll
// Photo mode (image=<side-by-side still>): the rig shows a real stereo photo, panned
// slowly so there is motion for the eye-sync check, with the same faults applied.
// --test-signal[=WxH@fps][,image=<path>] and the MEDIAPLAYER_TEST_* env vars set it
// (MEDIAPLAYER_TEST_ROLL is read by Open itself).
//
// The wire format (#238), read by Open:
//   default                         8-bit NV12, as always
//   MEDIAPLAYER_TEST_SIGNAL_V210=1  a v210 capture: each eye is packed into 10-bit v210 rows
//                                   (low 2 bits dithered, so the 10 bits matter), then either
//                                   published raw (PixFormat::V210, when the renderer's GPU
//                                   decode is up: MEDIAPLAYER_GPU_V210=1) or converted to P016
//                                   on the CPU first, as the DeckLink path does today.
//   MEDIAPLAYER_TEST_STILL=1        every frame is frame 0 (the timecode still runs), so two
//                                   runs can be compared pixel for pixel (atlas dumps).
// Raw v210 frames go through a ring of this class's own (LiveSourceBase has no v210 publish
// yet; phase 2 adds it with the DeckLink lanes, after the 0.9.7 direct-pair work).
#pragma once

#include "pro/LiveSourceBase.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp {

struct TestSignalConfig {
    int eyeW = 1920, eyeH = 1080;  // per eye; the frame is 2*eyeW x eyeH
    double fps = 60.0;
    float vShiftPx = 0.0f;
    int eyeDelay = 0;
    float gainR = 1.0f;
    float depthScale = 1.0f;       // multiplies every disc's disparity
    float rollDeg = 0.0f;          // right-eye roll, degrees (+ = clockwise on screen)
    std::string startTimecode = "01:00:00:00";
    std::string image;             // photo mode: a side-by-side stereo still

    // "1920x1080@60", ",image=<path>" (every part may be omitted; the path runs to the
    // end of the spec). False on a malformed spec.
    bool ParseSpec(const std::string& spec);

    // Photo mode: both eyes of the still, scaled to cover the eye with a margin for the
    // pan, faults baked into the right eye. Built by Open() (or a test).
    struct Plate {
        int margin = 0;
        livepix::Nv12 left, right;  // (eyeW + 2*margin) x (eyeH + 2*margin) each
    };
    std::shared_ptr<const Plate> plate;
    static std::shared_ptr<const Plate> BuildPlate(const TestSignalConfig& cfg, std::string* err);
};

class TestSignalSource : public LiveSourceBase {
public:
    ~TestSignalSource() override;
    bool Open(const TestSignalConfig& cfg);
    void Stop() override;
    const char* Kind() const override { return "test"; }
    FrameRing& Ring() override;
    LiveStats GetStats() const override;
    // What the worker publishes: "nv12", "v210 -> P016 (CPU)" or "v210 (raw)".
    const char* WireName() const;
    int LayoutHint() const override { return 1; }  // (int)StereoLayout::SbsFull

    // The scene's disparities, as a fraction of the eye width (negative = in front of
    // the screen), so tests can check what the depth measurement finds.
    static constexpr int kDiscs = 4;
    static const float kDisparity[kDiscs];

    // Draw frame n of the scene (both eyes, SBS-full NV12, 2*eyeW x eyeH) - what the
    // worker publishes, exposed so tests can check the rig measurements against it.
    static void Render(const TestSignalConfig& cfg, int64_t n, livepix::Nv12& out);

    // One eye (w x h) of an NV12 frame at column x0, as a card's v210 rows (RowBytes(w) bytes
    // each): luma and chroma << 2 with a fixed 2-bit pattern in the low bits that differs
    // between neighbouring rows, so a decode's 4:2:2 -> 4:2:0 rounding is exercised.
    static void PackEyeV210(const livepix::Nv12& src, int x0, int w, int h, uint8_t* dst);

private:
    enum class Wire { Nv12, V210Cpu, V210Raw };
    void Worker();
    void PublishV210(const livepix::Nv12& frame, double captureSec, const char* tc);
    TestSignalConfig cfg_;
    Wire wire_ = Wire::Nv12;
    bool still_ = false;
    std::atomic<bool> raw_{false};  // Ring() is rawRing_ (the render thread reads it)
    // The raw v210 arm's ring and stats (see the file comment).
    mutable FrameRing rawRing_;
    mutable std::mutex rawMutex_;
    LiveStats rawStats_;
    double rawWindowStart_ = 0.0;
    uint64_t rawWindowCount_ = 0;
    uint64_t rawTrimmed0_ = 0, rawRepeated0_ = 0;
    // The CPU arm's scratch: each eye's v210 rows, and the P016 pair.
    std::vector<uint8_t> v210Eye_[2];
    livepix::P016 p016_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
};

}  // namespace mp
