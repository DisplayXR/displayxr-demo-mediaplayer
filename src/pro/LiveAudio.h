// SPDX-License-Identifier: Apache-2.0
//
// Live audio from a capture card (LOXR-870): the embedded SDI / HDMI audio a DeckLink input
// delivers with each video frame, played through the default output device while the live
// picture is up.
//
// OFF by default. It is turned on per input in the DeckLink setup dialog ("Capture audio"),
// which writes ",audio" / ",audio=<pair>" into the input's spec (pro/DeckLinkSpec.h). When it
// is off the card is never asked for audio (no EnableAudioInput), so it delivers none, no
// audio device is opened, and the frame callback's only extra work is one null-pointer test.
//
// What is played: one channel pair of the captured channels, as stereo. The card captures
// the fewest channels that hold that pair (2, 8 or 16; LiveAudioCaptureChannels), 48 kHz,
// 32-bit. Live audio follows the card, not a file clock: packets are queued as they arrive,
// and if the output device's clock runs slower than the card's and the queue grows past
// kLiveAudioMaxQueueMs, the queue is dropped back to empty (counted as an overflow) so the
// sound never drifts away from the picture.
//
// The pure helpers (channel count, pair extraction) are inline here so tests need no SDL.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mp {
class AudioMeter;
}

namespace mp::pro {

constexpr int kLiveAudioRate = 48000;     // SDI embedded audio is always 48 kHz
constexpr int kLiveAudioMaxPair = 8;      // channels 15-16
constexpr int kLiveAudioMaxQueueMs = 200; // more queued than this = the clocks drifted: drop it

// The channel count to capture so that pair `pair` (1 = ch 1-2, 2 = ch 3-4, ...) is in it:
// the card takes 2, 8 or 16. 0 for an invalid pair.
inline int LiveAudioCaptureChannels(int pair) {
    if (pair < 1 || pair > kLiveAudioMaxPair) return 0;
    if (pair == 1) return 2;
    if (pair <= 4) return 8;
    return 16;
}

// "ch 1-2" etc. for the dialog and the log.
inline std::string LiveAudioPairName(int pair) {
    return "ch " + std::to_string(pair * 2 - 1) + "-" + std::to_string(pair * 2);
}

// Copies pair `pair` out of `frames` interleaved samples of `channels` channels into `out`
// (2 * frames samples, interleaved stereo). Returns false (nothing written) when the pair is
// not inside `channels`.
inline bool ExtractStereoPair(const int32_t* in, int frames, int channels, int pair, int32_t* out) {
    const int l = pair * 2 - 2;
    if (!in || !out || frames < 0 || pair < 1 || l + 1 >= channels) return false;
    for (int i = 0; i < frames; ++i) {
        out[2 * i] = in[(size_t)i * (size_t)channels + (size_t)l];
        out[2 * i + 1] = in[(size_t)i * (size_t)channels + (size_t)l + 1];
    }
    return true;
}

// The output side: one SDL audio stream on the default playback device. Push() is called on
// the card's callback thread (one input only); SetMuted() and Stats() from any thread.
class LiveAudioSink {
public:
    struct Stats {
        int channels = 0;          // captured per frame
        int pair = 0;              // played
        uint64_t packets = 0;      // audio packets received
        uint64_t sampleFrames = 0; // sample frames received
        uint64_t overflows = 0;    // times the queue was dropped (output clock behind the card)
        uint64_t badPackets = 0;   // packets with no samples readable
    };

    // Opens the default playback device for stereo 48 kHz 32-bit. nullptr + `err` on failure
    // (no SDL audio, no output device); the capture then just runs silent.
    // `meter` (may be null) gets the levels of every captured channel (LOXR-871).
    static std::shared_ptr<LiveAudioSink> Open(int channels, int pair, bool muted, float volume,
                                               AudioMeter* meter, std::string& err);
    ~LiveAudioSink();
    LiveAudioSink(const LiveAudioSink&) = delete;
    LiveAudioSink& operator=(const LiveAudioSink&) = delete;

    // `frames` sample frames of `channels_` interleaved int32 (the card's packet).
    void Push(const void* samples, int frames);
    void PacketWithoutSamples() { ++bad_; }
    void SetMuted(bool m);
    void SetVolume(float v);  // 0..1 (LOXR-872); mute wins over it
    Stats GetStats() const;

private:
    LiveAudioSink() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void ApplyGain();
    int channels_ = 0;
    int pair_ = 0;
    AudioMeter* meter_ = nullptr;
    std::atomic<bool> muted_{false};
    std::atomic<float> volume_{1.0f};
    std::vector<int32_t> stereo_;  // card thread only
    std::atomic<uint64_t> packets_{0}, frames_{0}, overflows_{0}, bad_{0};
};

}  // namespace mp::pro
