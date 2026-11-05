// SPDX-License-Identifier: Apache-2.0
// Unit tests for the output channel choice (media/AudioLayout.h).
#include "media/AudioLayout.h"

#include <cstdio>

using namespace mp;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

int main() {
    // 5.1 needs both ends.
    CHECK(ChooseOutChannels(6, 6) == 6);
    CHECK(ChooseOutChannels(6, 8) == 6);
    CHECK(ChooseOutChannels(8, 6) == 6);
    // A stereo device gets a downmix, and a stereo source is never spread out.
    CHECK(ChooseOutChannels(6, 2) == 2);
    CHECK(ChooseOutChannels(2, 6) == 2);
    CHECK(ChooseOutChannels(1, 8) == 2);
    CHECK(ChooseOutChannels(4, 6) == 2);
    // A device that could not be queried is treated as stereo.
    CHECK(ChooseOutChannels(6, 0) == 2);
    CHECK(ChooseOutChannels(6, -1) == 2);

    CHECK(DeviceChannelsWithOverride(8, nullptr) == 8);
    CHECK(DeviceChannelsWithOverride(8, "") == 8);
    CHECK(DeviceChannelsWithOverride(8, "2") == 2);
    CHECK(DeviceChannelsWithOverride(2, "6") == 6);
    CHECK(DeviceChannelsWithOverride(2, "4") == 2);
    CHECK(DeviceChannelsWithOverride(2, "junk") == 2);

    if (g_fail == 0) std::printf("audio_layout: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
