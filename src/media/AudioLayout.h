// SPDX-License-Identifier: Apache-2.0
// Which channel count the player outputs. Kept free of FFmpeg and SDL so it is testable.
#pragma once

#include <cstdlib>

namespace mp {

// 5.1 only when the source has at least 6 channels AND the device can play them;
// everything else is stereo. A 7.1 source on a 5.1-or-better device is folded to 5.1.
// deviceChannels <= 0 means the device could not be queried: stereo is the safe answer.
inline int ChooseOutChannels(int sourceChannels, int deviceChannels) {
    return (sourceChannels >= 6 && deviceChannels >= 6) ? 6 : 2;
}

// MEDIAPLAYER_AUDIO_CHANNELS=2 forces the stereo downmix, =6 forces 5.1 output (SDL then
// converts to the device, which exercises the 5.1 path on stereo hardware). Any other
// value, or unset, returns the device count unchanged.
inline int DeviceChannelsWithOverride(int deviceChannels, const char* overrideValue) {
    if (overrideValue && *overrideValue) {
        const int v = std::atoi(overrideValue);
        if (v == 2 || v == 6) return v;
    }
    return deviceChannels;
}

}  // namespace mp
