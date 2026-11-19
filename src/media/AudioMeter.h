// SPDX-License-Identifier: Apache-2.0
//
// AudioMeter: per-channel peak and RMS levels of the audio being HEARD, for the Pro panel's
// meters (LOXR-871). Fed by the code that queues audio on the output (AudioPlayer for files,
// pro::LiveAudioSink for DeckLink), read by the UI once per frame.
//
// Timing: audio is queued on the output ahead of playback (a file up to ~0.3 s, live audio
// ~50 ms), so levels taken at queue time would run ahead of the sound. Each Add says how
// many frames were already queued before it; a block counts as heard once that many frames
// have played since, so the meters follow the speakers and freeze while playback is paused.
// The estimate is refreshed on each Add, so it lags by at most one push (~20 ms).
//
// It only measures while someone is looking: the Pro panel calls Watch() on every frame it
// draws the meters. With the Audio section closed, the panels hidden or the UI off, no
// Watch() arrives, and after kWatchMs every Add returns at once (one clock read) without
// touching the samples or the lock. Nothing feeds it unless a meter is attached (Pro only):
// the base player pays nothing.
// Header-only and pure (no SDL, no FFmpeg) so tests/audio_meter_test.cpp can check it.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>

namespace mp {

class AudioMeter {
public:
    static constexpr int kMaxChannels = 16;
    static constexpr size_t kMaxBlocks = 512;  // ~10 s of audio blocks: never grows past it
    static constexpr int64_t kWatchMs = 500;   // measuring stops this long after the last Watch()

    // The meters are on screen this frame: measure for the next kWatchMs.
    void Watch() { watchUntilMs_.store(NowMs() + kWatchMs, std::memory_order_relaxed); }
    bool Watching() const { return NowMs() < watchUntilMs_.load(std::memory_order_relaxed); }

    // Levels heard since the last Take(), linear (1.0 = full scale).
    struct Reading {
        bool any = false;  // some audio was heard since the last Take
        int channels = 0;
        float peak[kMaxChannels] = {};
        float rms[kMaxChannels] = {};
    };

    // `frames` interleaved samples of `channels` channels, in [-1, 1]. `queuedFrames`: frames
    // already queued on the output ahead of these, not heard yet.
    void AddFloat(const float* s, int frames, int channels, int64_t queuedFrames) {
        Add(s, frames, channels, queuedFrames, 1.0);
    }
    // The card's 32-bit integer samples.
    void AddInt32(const int32_t* s, int frames, int channels, int64_t queuedFrames) {
        Add(s, frames, channels, queuedFrames, 1.0 / 2147483648.0);
    }

    // The output's queue was dropped (seek, resync, stop): nothing queued will be heard.
    void Clear() {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
        heard_ = pushed_;
    }

    Reading Take() {
        Reading r;
        double sumsq[kMaxChannels] = {};
        int64_t frames = 0;
        std::lock_guard<std::mutex> lk(m_);
        while (!q_.empty() && q_.front().end <= heard_) {
            const Block& b = q_.front();
            r.any = true;
            r.channels = b.channels;
            for (int c = 0; c < b.channels; ++c) {
                r.peak[c] = std::max(r.peak[c], b.peak[c]);
                sumsq[c] += b.sumsq[c];
            }
            frames += b.frames;
            q_.pop_front();
        }
        if (frames > 0)
            for (int c = 0; c < r.channels; ++c) r.rms[c] = (float)std::sqrt(sumsq[c] / (double)frames);
        return r;
    }

private:
    struct Block {
        int64_t end = 0;  // the stream position (frames) just after this block
        int channels = 0;
        int frames = 0;
        float peak[kMaxChannels] = {};
        double sumsq[kMaxChannels] = {};
    };

    static int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    template <class T>
    void Add(const T* s, int frames, int channels, int64_t queuedFrames, double scale) {
        if (!Watching()) {  // nobody is looking: do nothing
            idle_.store(true, std::memory_order_relaxed);
            return;
        }
        if (!s || frames <= 0 || channels <= 0) return;
        if (idle_.exchange(false, std::memory_order_relaxed)) Clear();  // drop blocks from before
        Block b;
        b.channels = std::min(channels, kMaxChannels);
        b.frames = frames;
        for (int i = 0; i < frames; ++i) {
            const T* f = s + (size_t)i * (size_t)channels;
            for (int c = 0; c < b.channels; ++c) {
                const double v = (double)f[c] * scale;
                const double a = v < 0 ? -v : v;
                if (a > b.peak[c]) b.peak[c] = (float)a;
                b.sumsq[c] += v * v;
            }
        }
        std::lock_guard<std::mutex> lk(m_);
        // Everything but `queuedFrames` of what was pushed before this block has been heard.
        heard_ = std::max(heard_, pushed_ - std::max<int64_t>(0, queuedFrames));
        pushed_ += frames;
        b.end = pushed_;
        q_.push_back(b);
        if (q_.size() > kMaxBlocks) q_.pop_front();
    }

    std::mutex m_;
    std::deque<Block> q_;
    int64_t pushed_ = 0;  // frames added so far
    int64_t heard_ = 0;   // frames played so far (estimated at the last Add)
    std::atomic<int64_t> watchUntilMs_{0};  // not watched until the panel says so
    std::atomic<bool> idle_{false};
};

// dBFS of a linear level, floored at kFloorDb.
constexpr float kMeterFloorDb = -96.0f;
inline float MeterDb(float linear) {
    return linear > 0.0f ? std::max(kMeterFloorDb, 20.0f * std::log10(linear)) : kMeterFloorDb;
}

// What the meters draw, per channel: the readings with meter ballistics, so a bar is
// readable at the UI's frame rate. Peaks jump up and fall at kFallDbPerSec; a hold mark
// stays at the highest peak for kHoldSec, then falls; RMS is smoothed over ~0.3 s; a clip
// (a sample at full scale) lights for kClipSec. With no audio the bars fall to the floor.
struct MeterDisplay {
    static constexpr float kFallDbPerSec = 24.0f;
    static constexpr float kHoldSec = 2.0f;
    static constexpr float kClipSec = 3.0f;
    static constexpr float kRmsTau = 0.3f;
    static constexpr float kClipLinear = 0.999f;  // ~ -0.01 dBFS

    int channels = 0;
    float peakDb[AudioMeter::kMaxChannels];
    float rmsDb[AudioMeter::kMaxChannels];
    float holdDb[AudioMeter::kMaxChannels];
    float holdAge[AudioMeter::kMaxChannels];
    float clipAge[AudioMeter::kMaxChannels];  // < kClipSec: lit

    MeterDisplay() { Reset(); }
    void Reset() {
        channels = 0;
        for (int c = 0; c < AudioMeter::kMaxChannels; ++c) {
            peakDb[c] = rmsDb[c] = holdDb[c] = kMeterFloorDb;
            holdAge[c] = 0.0f;
            clipAge[c] = kClipSec;
        }
    }
    bool Clipping(int c) const { return clipAge[c] < kClipSec; }

    void Update(const AudioMeter::Reading& r, float dt) {
        if (r.any) channels = r.channels;
        const float fall = kFallDbPerSec * dt;
        const float k = dt >= kRmsTau ? 1.0f : dt / kRmsTau;
        for (int c = 0; c < AudioMeter::kMaxChannels; ++c) {
            const bool fed = r.any && c < r.channels;
            const float p = fed ? MeterDb(r.peak[c]) : kMeterFloorDb;
            const float q = fed ? MeterDb(r.rms[c]) : kMeterFloorDb;
            peakDb[c] = std::max(p, std::max(kMeterFloorDb, peakDb[c] - fall));
            rmsDb[c] = std::max(kMeterFloorDb, rmsDb[c] + (q - rmsDb[c]) * k);
            if (p >= holdDb[c]) {
                holdDb[c] = p;
                holdAge[c] = 0.0f;
            } else {
                holdAge[c] += dt;
                if (holdAge[c] > kHoldSec) holdDb[c] = std::max(peakDb[c], holdDb[c] - fall);
            }
            if (fed && r.peak[c] >= kClipLinear) clipAge[c] = 0.0f;
            else if (clipAge[c] < kClipSec) clipAge[c] += dt;
        }
    }
};

}  // namespace mp
