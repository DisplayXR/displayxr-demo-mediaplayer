// SPDX-License-Identifier: Apache-2.0
//
// Packing for the grade shader's push block (shaders/sbs.frag), kept free of Vulkan so it
// is unit-tested (tests/overlays_test.cpp).
#pragma once

#include <cstdint>

namespace mp {

// The push block is at Vulkan's guaranteed 128 bytes, so the wipe position and the display
// gamma share one word: unorm16 position | gamma x 4096 in the high 16 bits (0.25..4).
inline uint32_t PackWipeGamma(float pos, float gamma) {
    const float p = pos < 0.0f ? 0.0f : pos > 1.0f ? 1.0f : pos;
    const float g = gamma < 0.25f ? 0.25f : gamma > 4.0f ? 4.0f : gamma;
    return (uint32_t)(p * 65535.0f + 0.5f) | ((uint32_t)(g * 4096.0f + 0.5f) << 16);
}
inline void UnpackWipeGamma(uint32_t v, float& pos, float& gamma) {
    pos = (float)(v & 0xFFFFu) / 65535.0f;
    gamma = (float)(v >> 16) / 4096.0f;
}

}  // namespace mp
