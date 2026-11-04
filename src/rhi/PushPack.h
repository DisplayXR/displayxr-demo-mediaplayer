// SPDX-License-Identifier: Apache-2.0
//
// Packing for the grade shader's push block (shaders/sbs.frag), kept free of Vulkan so it
// is unit-tested (tests/overlays_test.cpp).
#pragma once

#include <cmath>
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

// The shader's `yuvMatrix` word (#134): bits 0-1 the Y'CbCr matrix (unchanged, so an SDR
// frame packs to exactly the matrix as before), bits 4-5 the HDR transfer (0 SDR, 1 HLG,
// 2 PQ, 3 scene-linear stored as PQ), bit 6 the highlight roll-off, bit 7 BT.2020 primaries,
// bit 8 the PQ shaper for an OCIO look (ToneMap.h HdrParams::pqShaper), bits 16-31 the
// source peak in nits. SDR ignores everything but the matrix.
inline int32_t PackColourWord(int matrix, int transfer, bool rollOff, bool bt2020, float peakNits,
                              bool pqShaper = false) {
    uint32_t w = (uint32_t)(matrix & 3);
    if (transfer <= 0 || transfer > 3) return (int32_t)w;
    const float pk = peakNits < 1.0f ? 1.0f : peakNits > 10000.0f ? 10000.0f : peakNits;
    w |= (uint32_t)transfer << 4;
    if (rollOff) w |= 1u << 6;
    if (bt2020) w |= 1u << 7;
    if (pqShaper) w |= 1u << 8;
    w |= (uint32_t)(pk + 0.5f) << 16;
    return (int32_t)w;
}

// The compare clip B's HDR tags (#169: an EXR or HDR video on the B side of the clip wipe
// was shown as raw PQ code), in bits both words leave free. viewFlags: bits 6-7 B's transfer
// (as above), bit 3 BT.2020 primaries. yuvMatrix: bit 2 the PQ shaper, bit 9 the roll-off,
// bits 10-15 B's peak on a log scale, 100..10000 nits in 63 steps of ~7.6 % (A's full 16-bit
// peak has no room left). An SDR B packs to 0 in both, so nothing changes without one.
struct CompareHdrBits {
    uint32_t viewFlags = 0;
    uint32_t yuvMatrix = 0;
};
inline int PackComparePeakCode(float peakNits) {
    const float pk = peakNits < 100.0f ? 100.0f : peakNits > 10000.0f ? 10000.0f : peakNits;
    return (int)(std::log(pk / 100.0f) / std::log(100.0f) * 63.0f + 0.5f);
}
inline float UnpackComparePeak(int code) { return 100.0f * std::pow(100.0f, (float)code / 63.0f); }
inline CompareHdrBits PackCompareHdr(int transfer, bool rollOff, bool bt2020, float peakNits, bool pqShaper) {
    CompareHdrBits b;
    if (transfer <= 0 || transfer > 3) return b;
    b.viewFlags = ((uint32_t)transfer << 6) | (bt2020 ? 1u << 3 : 0u);
    b.yuvMatrix = (pqShaper ? 1u << 2 : 0u) | (rollOff ? 1u << 9 : 0u) |
                  ((uint32_t)PackComparePeakCode(peakNits) << 10);
    return b;
}

}  // namespace mp
