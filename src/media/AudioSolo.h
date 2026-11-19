// SPDX-License-Identifier: Apache-2.0
//
// Channel solo for Pro's meters (LOXR-874): clicking a channel's meter plays that channel
// alone, to both front speakers, so any one of 2 / 5.1 / 8 / 16 channels can be listened to.
// The meters still show every channel as it comes in (they measure before the solo).
// Pure and header-only so tests/audio_meter_test.cpp can check it.
#pragma once

#include <cstdint>

namespace mp {

// The file player's interleaved float output, in place: channel `solo` goes to the front
// left and right (channels 0 and 1, also in 5.1's FL FR FC LFE BL BR order) and every other
// channel is silenced. solo < 0 or out of range: unchanged (no solo).
inline void SoloFloatInPlace(float* buf, int frames, int channels, int solo) {
    if (!buf || frames <= 0 || channels <= 0 || solo < 0 || solo >= channels) return;
    for (int i = 0; i < frames; ++i) {
        float* f = buf + (size_t)i * (size_t)channels;
        const float v = f[solo];
        for (int c = 0; c < channels; ++c) f[c] = c < 2 ? v : 0.0f;
    }
}

// The card's interleaved int32 packet -> stereo with channel `solo` on both sides. False
// (nothing written) when `solo` is not one of the captured channels.
inline bool ExtractSoloStereo(const int32_t* in, int frames, int channels, int solo, int32_t* out) {
    if (!in || !out || frames < 0 || solo < 0 || solo >= channels) return false;
    for (int i = 0; i < frames; ++i) out[2 * i] = out[2 * i + 1] = in[(size_t)i * (size_t)channels + (size_t)solo];
    return true;
}

}  // namespace mp
