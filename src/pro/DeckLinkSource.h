// SPDX-License-Identifier: Apache-2.0
//
// DeckLinkSource — a Blackmagic DeckLink / UltraStudio SDI or HDMI input as a live
// source, read natively through the Desktop Video SDK (needs Blackmagic "Desktop
// Video" installed). Ported from the Open RV DisplayXR Edition's MovieLive reader.
//
// Stereo:
//   Mono / Sbs  one input; Sbs is a side-by-side signal on one cable (layout guessed
//               from the frame shape, like a camera)
//   Dual        the card's dual-stream 3D mode: left and right on its two 3D inputs,
//               delivered by the card as one matched pair
//   Pair        two separate inputs (two DeckLink devices, e.g. the connectors of a
//               DeckLink Duo/Quad), paired by SMPTE timecode: the newest timecode both
//               eyes have is shown, so a jam-synced rig stays frame-accurate. Without
//               matching timecode it falls back to the newest frame of each and reports
//               the offset.
//   Auto        one input; switches to Dual when the card detects a dual-stream signal
//
// Format detection follows the signal (resolution, rate, YUV/RGB) when the card can.
// Frames are converted to NV12 (both eyes side by side for Dual/Pair) and published
// into the ring; the source timecode and capture time travel with each frame.
#pragma once

#include "pro/LiveSourceBase.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace mp {

struct DeckLinkConfig {
    enum class Stereo { Auto, Mono, Sbs, Dual, Pair };
    std::string device = "0";  // index or part of the name
    std::string rightDevice;   // Pair: the right eye's input (index or name part)
    std::string input;         // "" (as set in Desktop Video) | sdi | hdmi
    std::string mode;          // starting display mode FourCC, e.g. "Hp25"; "" = 1080p25
    Stereo stereo = Stereo::Auto;

    static bool ParseStereo(const std::string& s, Stereo& out);
    static const char* StereoName(Stereo s);
};

class DeckLinkSource : public LiveSourceBase {
public:
    ~DeckLinkSource() override;

    // True when this build has the DeckLink SDK compiled in.
    static bool Available();
    // "0: DeckLink Duo (1)" lines; empty when there are none (or no driver).
    static std::vector<std::string> Enumerate();
    static void PrintDeviceList(FILE* out);

    // Non-blocking: spawns the worker, which finds the device(s) and starts capture.
    // GetState(): Opening -> Streaming | Stalled (no signal) | Failed (LastError()).
    bool Open(const DeckLinkConfig& cfg);
    void Stop() override;
    const char* Kind() const override { return "decklink"; }
    int LayoutHint() const override;

    const DeckLinkConfig& Config() const { return cfg_; }

private:
    friend struct DeckLinkCapture;  // the SDK side (DeckLinkSource.cpp) publishes through us
    void Worker();
    DeckLinkConfig cfg_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> dualActive_{false};  // Auto switched to the card's dual-stream mode
};

}  // namespace mp
