// SPDX-License-Identifier: Apache-2.0
//
// DeckLinkFrames — the hardware-free half of Media Player Pro's DeckLink input:
// pixel conversion from the formats a DeckLink card delivers to the NV12 the renderer
// already consumes, and the pairing of two SDI inputs (left + right camera) into one
// stereo frame. Kept apart from DeckLinkSource (which needs the Blackmagic SDK and a
// card) so it builds everywhere and is unit-tested in tests/decklink_frames_test.cpp.
#pragma once

#include <cstdint>

namespace mp::pro::dl {

// What a DeckLink input delivers for YUV signals.
enum class SrcFormat {
    UYVY8,  // bmdFormat8BitYUV ('2vuy'): U0 Y0 V0 Y1 per 2 pixels
    V210,   // bmdFormat10BitYUV ('v210'): 6 pixels in 4 little-endian 32-bit words
};

// Bytes per row of a v210 frame of `width` pixels (rows pad to 48 pixels / 128 bytes).
int V210RowBytes(int width);

// Convert one w x h source frame into the region x in [dstX, dstX + w) of an NV12
// frame whose Y and interleaved-UV planes are both `dstStride` bytes wide (NV12: the
// UV plane has h/2 rows). `w`, `h` and `dstX` must be even. 10-bit samples keep their
// top 8 bits. Chroma is averaged over each 2-row pair (4:2:2 -> 4:2:0). The output
// stays limited range, which is what SDI carries.
void ToNV12(SrcFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, uint8_t* dstY,
            uint8_t* dstUV, int dstStride, int dstX);

// Pairs frames from two inputs (eye 0 = left, eye 1 = right) by the card's hardware
// reference timestamp. Two genlocked cameras land within a fraction of a frame of each
// other; anything further apart is a different moment and must not be shown as a
// stereo pair (that is exactly the eye-sync error a rig check looks for).
class StereoPairer {
public:
    struct Stats {
        uint64_t pairs = 0;
        uint64_t unpaired = 0;    // frames dropped because the other eye had no match
        int64_t lastSkew = 0;     // right - left of the last pair, in timestamp units
        int64_t maxAbsSkew = 0;
    };

    // `tolerance`: the largest |left - right| still accepted as one moment (use half a
    // frame duration, in the same units as the timestamps).
    explicit StereoPairer(int64_t tolerance = 0) : tol_(tolerance) {}
    void SetTolerance(int64_t t) { tol_ = t; }
    void Reset();

    // Offer eye `eye`'s newest frame. Returns true when it completes a pair with the
    // other eye's pending frame; both are then consumed. A pending frame older than the
    // tolerance is discarded (counted as unpaired) and the new one waits instead.
    bool Offer(int eye, int64_t timestamp);

    const Stats& GetStats() const { return stats_; }

private:
    int64_t tol_;
    bool pending_[2] = {false, false};
    int64_t ts_[2] = {0, 0};
    Stats stats_;
};

}  // namespace mp::pro::dl
