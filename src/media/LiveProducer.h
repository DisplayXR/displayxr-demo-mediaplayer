// SPDX-License-Identifier: Apache-2.0
//
// LiveProducer — what App's live mode needs from a live picture source, whatever it
// is: the USB/UVC camera (CameraSource, #90), a Blackmagic DeckLink SDI/HDMI input
// (DeckLinkSource) or the built-in test signal (TestSignalSource). Each publishes into
// its own FrameRing on its own thread; App acquires the newest frame per render.
#pragma once

#include "media/FrameRing.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace mp {

enum class LiveState {
    Closed,
    Opening,
    Streaming,
    Stalled,  // opened, but no frame for 2 s (a capture input with no signal)
    Lost,     // unplugged / device error
    Denied,   // OS privacy setting or a declined permission prompt
    Failed,   // could not open (LastError() says why)
};

inline const char* LiveStateName(LiveState s) {
    switch (s) {
        case LiveState::Closed: return "closed";
        case LiveState::Opening: return "opening";
        case LiveState::Streaming: return "streaming";
        case LiveState::Stalled: return "stalled";
        case LiveState::Lost: return "lost";
        case LiveState::Denied: return "denied";
        case LiveState::Failed: return "failed";
    }
    return "?";
}

struct LiveStats {
    float deliveredFps = 0.f;     // frames published to the ring per second (1 s window)
    uint64_t published = 0;
    uint64_t dropped = 0;         // frames drained-and-discarded (only the newest is shown)
    double lastPublishSec = 0.0;  // steady_clock seconds of the last Publish (consumer computes age)
    int w = 0, h = 0, fpsReq = 0;
    bool fullRange = false;
    bool mjpgPath = false;        // camera: MJPG decode rung in use
    int decoders = 0;             // camera: decoder-pool size when mjpgPath
    // Two-input stereo (DeckLink pair / dual-stream, test signal): how far apart the two
    // eyes' frames were when paired, in frames (0 = same instant). -1 = not measured.
    int eyeOffsetFrames = -1;
    double fps = 0.0;             // the signal's nominal rate (DeckLink / test), 0 = unknown
    int noSignalEyes = 0;         // DeckLink: bit 0 = left / only input dark, bit 1 = right
    uint64_t lateFrames = 0;      // DeckLink: card frames skipped because the converter was
                                  // still busy with the previous one (the newest is kept)
};

class LiveProducer {
public:
    virtual ~LiveProducer() = default;

    virtual LiveState GetState() const = 0;
    virtual std::string LastError() const = 0;
    virtual const std::string& DeviceName() const = 0;
    virtual LiveStats GetStats() const = 0;
    virtual FrameRing& Ring() = 0;
    virtual void Stop() = 0;
    // "camera" | "decklink" | "test": words for logs, toasts and the agent tools.
    virtual const char* Kind() const = 0;
    // The frames' stereo packing when the source knows it (two inputs composed side by
    // side are SBS-full; a mono input is mono). -1 = unknown: guess from the aspect.
    virtual int LayoutHint() const { return -1; }

    // Seconds on the steady clock (the timebase of LiveStats::lastPublishSec and of
    // FrameRing::Frame::captureSec).
    static double NowSeconds() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
};

}  // namespace mp
