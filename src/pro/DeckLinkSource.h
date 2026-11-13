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
#include <condition_variable>
#include <cstdint>
#include <mutex>
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
    // Why there is nothing to open: "" when a device is present, else no SDK in this
    // build / Blackmagic Desktop Video driver not installed / no card found.
    static std::string UnavailableReason();
    // The installed Desktop Video's API version ("16.0.1"); "" when unknown, no driver
    // or no SDK in this build. --decklink-list prints it (station acceptance E2 reads it).
    static std::string DriverVersionText();
    // The bit depth of the last frame captured (10, or 8 after a refused 10-bit or for RGB);
    // 0 before the first frame. The station's capture stats report it.
    static int LastBitDepth();
    // Bit depth the capture loses to the pixel format the card took after a format change
    // (e.g. "12-bit RGB signal captured as 10-bit RGB (r210): 2 bits of depth lost"); "" when
    // none. The Station panel shows it and get_rig_status reports it (#169).
    std::string FormatWarning() const;

    // Two-input stereo: how far apart the paired frames were on the card's hardware
    // reference clock, and how many frames never made it into a pair.
    struct PairStats {
        uint64_t pairs = 0;
        uint64_t unpaired = 0;
        double lastSkewMs = 0.0, maxAbsSkewMs = 0.0;
        bool hwClockPairing = false;  // no timecode: paired by the card's hardware clock
        bool timecode = false;        // paired by SMPTE timecode
        bool separateCards = false;   // the two inputs are on different cards: two clocks,
                                      // so without timecode only the newest frames pair
        bool strict = false;          // matched pairs only (one card; MEDIAPLAYER_DECKLINK_STRICT_PAIRS)
        uint64_t mismatchRefused = 0; // strict: pairs NOT shown because the eyes were not one instant
        // Direct pairing (pro/DirectPair.h; MEDIAPLAYER_DECKLINK_DIRECT_PAIR): each eye converted
        // straight into its half of the ring frame, and a pair of two such halves published
        // whole. `pairs` = directPairs + composedPairs + raw v210 pairs (CaptureStats::rawPublished).
        bool directPair = false;      // on for this capture (the switch, and a queue-mode ring)
        uint64_t directPairs = 0;     // published whole: no compose copy
        uint64_t composedPairs = 0;   // composed from two eyes, the old path (all of them when off)
        uint64_t directNoFrame = 0;   // frames with a key that found no ring frame to spare
        uint64_t directSkipped = 0;   // decisions not shown: an eye's pixels had already gone out whole
    };
    PairStats GetPairStats() const;

    // ---- The capture path's levers and stage timings (pro/CaptureLevers.h,
    // docs/CAPTURE-LEVERS.md): what this capture actually runs, and where its time goes.
    struct Stage {
        uint64_t count = 0;            // samples in the last full window
        double avgMs = 0.0, maxMs = 0.0;
    };
    // A stage's timings, in windows of ~10 s: the last full window is what is reported (a
    // window is closed by the first sample after it ends). Thread-safe.
    class StageClock {
    public:
        void Add(double ms, double now) {
            std::lock_guard<std::mutex> lk(m_);
            if (start_ <= 0.0) start_ = now;
            if (now - start_ >= kWindowSec) {
                last_ = {cur_.n, cur_.n ? cur_.sum / (double)cur_.n : 0.0, cur_.max};
                cur_ = {};
                start_ = now;
            }
            ++cur_.n;
            cur_.sum += ms;
            if (ms > cur_.max) cur_.max = ms;
        }
        Stage Last() const {
            std::lock_guard<std::mutex> lk(m_);
            return last_;
        }
        void Reset() {
            std::lock_guard<std::mutex> lk(m_);
            cur_ = {};
            last_ = {};
            start_ = 0.0;
        }
        static constexpr double kWindowSec = 10.0;

    private:
        struct Acc {
            uint64_t n = 0;
            double sum = 0.0, max = 0.0;
        };
        mutable std::mutex m_;
        Acc cur_;
        Stage last_;
        double start_ = 0.0;
    };
    struct CaptureStats {
        // in effect for this capture (a lever asked for but not in effect says why in `notes`)
        bool rawV210 = false;     // MEDIAPLAYER_DECKLINK_RAW_V210 (+ the GPU decode up)
        bool allocator = false;   // MEDIAPLAYER_DECKLINK_ALLOCATOR (the card took it)
        bool hold = false;        // MEDIAPLAYER_DECKLINK_HOLD
        bool zeroCopy = false;    // MEDIAPLAYER_ZERO_COPY_UPLOAD (+ raw, hold, the renderer's import)
        int allocNode = -1;       // MEDIAPLAYER_DECKLINK_ALLOC_NODE
        int pinGroup[2] = {-1, -1};  // MEDIAPLAYER_CAPTURE_PIN: lane e's L3 group (-1 = not pinned)
        bool pinned[2] = {false, false};
        int cacheGroups = 0;      // the CPU's last-level-cache groups (platform/ThreadPin.h)
        int convertThreads = 0;   // per input
        std::string notes;
        // since the capture opened
        uint64_t rawPublished = 0;  // frames / pairs published as raw v210
        uint64_t rawSkipped = 0;    // pairs not shown: an eye's rows had already gone out
        uint64_t rawFallback = 0;   // frames converted on the CPU although raw was on (not v210)
        uint64_t zeroCopyPublished = 0;  // frames / pairs whose rows went to the GPU in the card's buffers
        uint64_t zeroCopyFallback = 0;   // eyes copied out although zero-copy was on (no held block)
        uint64_t allocBuffers = 0, allocMB = 0, allocCalls = 0, allocRefused = 0;
        int heldNow = 0, heldPeak = 0;
        // the last ~10 s window, per eye unless said: the card callback's copy (or hold), the
        // lane's convert (or raw move), the wait to claim a pairing slot, the publish (per frame
        // shown)
        Stage callback, convert, claimWait, publish;
    };
    CaptureStats GetCaptureStats() const;
    bool IsPair() const { return cfg_.stereo == DeckLinkConfig::Stereo::Pair && GetState() != LiveState::Closed; }
    static void PrintDeviceList(FILE* out);

    // Non-blocking: spawns the worker, which finds the device(s) and starts capture.
    // GetState(): Opening -> Streaming | Stalled (no signal) | Failed (LastError()).
    // Refuses the same device for both eyes; needs the installed Desktop Video to be at
    // least the SDK version this build used (14.3+ reads frames through interfaces an
    // older driver lacks).
    bool Open(const DeckLinkConfig& cfg);
    void Stop() override;
    const char* Kind() const override { return "decklink"; }
    int LayoutHint() const override;

    const DeckLinkConfig& Config() const { return cfg_; }

private:
    friend struct DeckLinkCapture;  // the SDK side (DeckLinkSource.cpp) publishes through us
    void Worker();
    void SetFormatWarning(int eye, const std::string& text);
    mutable std::mutex warnMutex_;
    std::string formatWarning_[2];  // per input
    mutable std::mutex pairMutex_;
    PairStats pairStats_;
    mutable std::mutex captureMutex_;
    CaptureStats captureStats_;  // config + counters (the stage clocks below hold the timings)
    StageClock clockCallback_, clockConvert_, clockClaim_, clockPublish_;
    DeckLinkConfig cfg_;
    std::thread worker_;
    std::mutex stopMutex_;
    std::condition_variable stopCv_;  // Stop() wakes the worker's watchdog loop at once
    std::atomic<bool> stop_{false};
    std::atomic<bool> dualActive_{false};  // Auto switched to the card's dual-stream mode
};

}  // namespace mp
