// SPDX-License-Identifier: Apache-2.0
//
// TestSignalSource — a synthetic stereo "rig" for testing live monitoring without a
// camera or capture card: moving discs at known depths in front of a gradient, both
// eyes composed side by side (SBS-full) exactly as a two-input SDI pair arrives, with
// a running SMPTE timecode. Faults a real rig has can be dialled in, so the rig checks
// have something to find:
//   vShiftPx     right eye moved down by this many pixels (vertical misalignment)
//   eyeDelay     right eye shows the scene this many frames late (eye sync)
//   gainR        right eye brightness x this (colour match)
// --test-signal[=WxH@fps] and the MEDIAPLAYER_TEST_* env vars (main.cpp) set it.
#pragma once

#include "pro/LiveSourceBase.h"

#include <atomic>
#include <string>
#include <thread>

namespace mp {

struct TestSignalConfig {
    int eyeW = 1920, eyeH = 1080;  // per eye; the frame is 2*eyeW x eyeH
    double fps = 60.0;
    float vShiftPx = 0.0f;
    int eyeDelay = 0;
    float gainR = 1.0f;
    float depthScale = 1.0f;       // multiplies every disc's disparity
    std::string startTimecode = "01:00:00:00";

    // "1920x1080@60" (any part may be omitted). False on a malformed spec.
    bool ParseSpec(const std::string& spec);
};

class TestSignalSource : public LiveSourceBase {
public:
    ~TestSignalSource() override;
    bool Open(const TestSignalConfig& cfg);
    void Stop() override;
    const char* Kind() const override { return "test"; }
    int LayoutHint() const override { return 1; }  // (int)StereoLayout::SbsFull

    // The scene's disparities, as a fraction of the eye width (negative = in front of
    // the screen), so tests can check what the depth measurement finds.
    static constexpr int kDiscs = 4;
    static const float kDisparity[kDiscs];

    // Draw frame n of the scene (both eyes, SBS-full NV12, 2*eyeW x eyeH) - what the
    // worker publishes, exposed so tests can check the rig measurements against it.
    static void Render(const TestSignalConfig& cfg, int64_t n, livepix::Nv12& out);

private:
    void Worker();
    TestSignalConfig cfg_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
};

}  // namespace mp
