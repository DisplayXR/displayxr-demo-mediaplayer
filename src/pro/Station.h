// SPDX-License-Identifier: Apache-2.0
//
// pro::Station — the monitoring-station half of Media Player Pro (docs/PRO.md, Next 1-4):
// named live inputs (Blackmagic DeckLink SDI/HDMI, the synthetic test rig, the trunk's
// camera and streams), the source timecode and measured capture -> screen delay, the rig
// checks with their 60-second history, and snapshots with notes. ProController owns one
// and forwards keys, frames and inspector sections to it.
//
// Keys (none used by the trunk or ProController): T timecode, R rig strip, N snapshot,
// J next input.
#pragma once

#include "media/FrameRing.h"
#include "media/LiveProducer.h"
#include "media/StereoTypes.h"
#include "pro/DeckLinkSource.h"
#include "pro/RigCheck.h"
#include "pro/TestSignalSource.h"

#include <functional>
#include <string>
#include <vector>

namespace mp::pro {

struct StationHost {
    // Make `p` (opened) the live source - App::EnterLive.
    std::function<void(LiveProducer* p, const std::string& name)> enterLive;
    std::function<void()> stopLive;  // leave live mode (a re-opened input failed)
    // The trunk's own live sources, by input spec.
    std::function<bool(const std::string& selector)> openCamera;
    std::function<bool(const std::string& url)> openUrl;
    // Atlas capture (both eyes as shown, grade applied): the path prefix for a stem, and
    // the capture itself (the runtime writes <prefix>_atlas.png). False when unsupported.
    std::function<std::string(const std::string& stem)> capturePrefix;
    std::function<bool(const std::string& prefix)> captureAtlas;
    std::function<void(const std::string&)> toast;
    std::function<double()> displayPeriodMs;  // one panel frame, for the delay readout
};

struct StationOptions {
    std::vector<std::string> inputs;  // "Name=spec" (--input / --decklink / --test-signal)
    rig::Budget budget;
    bool timecode = true;
    TestSignalConfig testCfg;         // faults from MEDIAPLAYER_TEST_*
    bool startFirstInput = true;      // false when the command line already opened media
};

class Station {
public:
    ~Station();
    void Init(StationHost host, const StationOptions& opt);
    void Shutdown();

    // Keys (SDL keycode); true when consumed. Actions that change the player run in Tick().
    bool HandleKey(int sdlKey);
    void Tick();  // per frame, from ProController::Tick (outside the SDL event hook)
    // Every frame the trunk uploads (live or file): timecode, delay, rig checks.
    void OnFrame(const FrameRing::Frame& f, StereoLayout layout, bool live, int eyeOffsetFrames);

    bool SwitchInput(int index);
    // Agent tools for the station (called from ProController::DispatchTool).
    bool DispatchTool(const std::string& tool, const std::string& argsJson, std::string& result, bool& success);
    bool TakeSnapshot(const std::string& note);

    // Inspector sections and the bottom history strip (ImGui).
    void BuildInputsSection(const std::string& liveName, bool live);
    void BuildSignalLines(bool live);
    void BuildRigSection(bool stereo);
    void BuildSnapshotsSection();
    void BuildHistoryStrip(float alpha, bool stereo);
    // DeckLink setup window: pick the left / right inputs, connection, stereo packing and
    // starting format, then Open (adds or replaces the "DeckLink" input and switches to it).
    void OpenDeckLinkDialog();
    void BuildDeckLinkDialog();
    bool ShowTimecode() const { return showTimecode_; }
    std::string InputName() const { return inputIndex_ >= 0 && inputIndex_ < (int)inputs_.size() ? inputs_[inputIndex_].name : std::string(); }
    size_t SnapshotCount() const { return snapshots_.size(); }

    bool ShowHistory() const { return showHistory_; }
    rig::Result RigLast() const { return rig_.Last(); }
    rig::Budget RigBudget() const { return rig_.GetBudget(); }
    const std::string& Timecode() const { return timecode_; }
    double DelayMs() const { return delayMs_; }

private:
    void LogNewAlerts();
    void SaveNote(size_t i);

    StationHost host_;
    StationOptions opt_;
    DeckLinkSource decklink_;
    TestSignalSource testSignal_;
    rig::RigMonitor rig_;
    struct Input {
        std::string name, spec;
    };
    std::vector<Input> inputs_;
    int inputIndex_ = -1;
    bool showTimecode_ = true;
    bool showHistory_ = true;
    bool pendingSnapshot_ = false, pendingSwitch_ = false;
    std::string pendingSnapshotNote_;
    // DeckLink setup window
    bool dlgOpen_ = false;
    std::vector<std::string> dlgDevices_;
    std::string dlgReason_;
    int dlgLeft_ = 0, dlgRight_ = 0;  // device list index; right 0 = "none (one cable)"
    int dlgConn_ = 0;                 // 0 as set in Desktop Video, 1 SDI, 2 HDMI
    int dlgStereo_ = 0;               // one cable: 0 auto, 1 side by side, 2 dual-stream 3D, 3 mono
    char dlgMode_[8] = "";            // starting format FourCC; empty = 1080p25 (auto-detect follows)
    std::string pendingDeckLinkSpec_;
    int testFrames_ = 0, testSnapshotFrame_ = -1;
    std::string timecode_;
    double delayMs_ = 0.0;
    double alertsLoggedUntil_ = 0.0;
    struct Snapshot {
        std::string image, notePath, timecode, clock;
        std::string prefix;        // capture prefix; the runtime picks the image's suffix
        double takenSec = 0.0;     // until the image is found (Tick), `image` is a guess
        bool imageFound = false;
        int eyeSync = -1;          // frames between the paired inputs (-1 unknown)
        char note[160] = {0};
        rig::Result rig;
    };
    std::vector<Snapshot> snapshots_;
};

}  // namespace mp::pro
