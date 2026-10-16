// SPDX-License-Identifier: Apache-2.0
//
// DeckLinkSource — Blackmagic DeckLink / UltraStudio SDI/HDMI input for Media Player
// Pro, as a frame producer for the same FrameRing the live camera uses. The renderer
// cannot tell it from the camera: it publishes NV12 (limited range, as SDI carries),
// so UploadYUV + the SBS slicing + eye swap + the Pro grade all apply unchanged.
//
// Two stereo modes:
//   Single  one input carrying side-by-side stereo (e.g. DaVinci Resolve's stereo
//           output, or a rig processor's SBS feed). Published as-is.
//   Dual    two inputs, left camera + right camera ("two-cable 3D"). Each input
//           converts into its own eye buffer; StereoPairer matches them by the card's
//           hardware reference timestamp, and only a matched pair is published, as
//           one full-width side-by-side frame (so the layout settles to SBS-full).
//
// Format: auto-detected when the card supports it (VideoInputFormatChanged ->
// re-enable with the detected mode, 8- or 10-bit YUV). 10-bit is converted to 8-bit
// for now (the panel path is 8-bit; docs/PRO.md).
//
// Build: the real implementation needs the Blackmagic DeckLink SDK, which is a
// registration download and is never committed. Configure with
//   -DMP_DECKLINK_SDK_DIR=<dir containing DeckLinkAPI.idl>
// and CMake runs MIDL on it (Windows). Without it this class is a stub whose
// Available() is false, so the Pro build, CI and the inspector all still work.
#pragma once

#include "media/FrameRing.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mp::pro {

class DeckLinkSource {
public:
    struct Device {
        int index = 0;
        std::string name;
        bool formatDetection = false;
    };
    enum class Mode { Single, Dual };
    enum class State { Closed, Opening, Streaming, NoSignal, Failed };
    struct Stats {
        float fps = 0.f;              // frames published per second
        uint64_t published = 0;
        uint64_t unpaired = 0;        // dual: frames dropped with no matching other eye
        double lastSkewMs = 0.0;      // dual: right - left of the last pair
        double maxSkewMs = 0.0;
        int width = 0, height = 0;    // per input
        std::string modeName;         // e.g. "2160p23.98"
        bool tenBit = false;
        std::string timecode;         // RP188 / VITC of the newest frame, "" if none
        double lastPublishSec = 0.0;  // steady seconds (CameraSource::NowSeconds timebase)
    };

    DeckLinkSource();
    ~DeckLinkSource();
    DeckLinkSource(const DeckLinkSource&) = delete;
    DeckLinkSource& operator=(const DeckLinkSource&) = delete;

    // True when this build has the SDK compiled in. (A missing driver or card shows up
    // as an empty Enumerate() and a reason in Unavailable().)
    static bool CompiledIn();
    // Lists DeckLink devices (each SDI/HDMI connector of a multi-input card is its own
    // device). Empty when the SDK, the Desktop Video driver or a card is missing.
    static std::vector<Device> Enumerate();
    // Why Enumerate() is empty, for the inspector: "built without the DeckLink SDK",
    // "Desktop Video driver not installed", "no DeckLink device found".
    static std::string Unavailable();

    // Start capturing. Single: `left` is the SBS input, `right` ignored. Non-blocking
    // past the SDK calls; state goes Opening -> Streaming | NoSignal | Failed.
    bool Open(Mode mode, int left, int right);
    void Stop();

    State GetState() const { return state_.load(); }
    static const char* StateName(State s);
    std::string LastError() const;
    std::string Description() const;  // "DeckLink 8K Pro (1) + (2)"
    Stats GetStats() const;
    FrameRing& Ring() { return ring_; }

    // --- called from the SDK callback threads (implementation detail) ---
    struct Impl;

private:
    FrameRing ring_;
    std::atomic<State> state_{State::Closed};
    std::unique_ptr<Impl> impl_;
};

}  // namespace mp::pro
