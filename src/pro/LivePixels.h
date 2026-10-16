// SPDX-License-Identifier: Apache-2.0
//
// Pixel helpers for live capture sources: an SDI/HDMI card delivers 8-bit 4:2:2 YUV
// (UYVY) or 8-bit RGB (BGRA); the renderer takes NV12. Each eye is converted straight
// into its place in a side-by-side NV12 frame, so a two-input pair costs one pass and
// no extra copy. Pure functions, unit-tested (tests/live_pixels_test.cpp).
#pragma once

#include <cstdint>
#include <vector>

namespace mp::livepix {

// A tight NV12 frame: Y rows (width bytes each), then interleaved UV rows (width bytes,
// height/2 rows). width and height are even.
struct Nv12 {
    int width = 0, height = 0;
    std::vector<uint8_t> y, uv;
    void Resize(int w, int h);
};

// Convert one eye (w x h) into dst at column x0 (even). UYVY: Cb Y0 Cr Y1 per 2 pixels.
// 4:2:2 -> 4:2:0 averages each vertical chroma pair.
void UyvyToNv12(const uint8_t* src, int srcRowBytes, int w, int h, Nv12& dst, int x0);
// BGRA (an RGB 4:4:4 signal) -> limited-range BT.709 NV12 (BT.601 below 720 lines).
void BgraToNv12(const uint8_t* src, int srcRowBytes, int w, int h, Nv12& dst, int x0);
// Copy one eye of dst (columns [x0, x0+w)) onto columns [x1, x1+w) (repeat an eye).
void CopyEye(Nv12& dst, int x0, int x1, int w);

// The 16-bit twin (FrameRing P016): NV12 layout, samples MSB-aligned in 16 bits. The
// DeckLink path captures 10-bit and keeps it to the GPU, so the look grades at 10 bits.
struct P016 {
    int width = 0, height = 0;
    std::vector<uint16_t> y, uv;  // y: width*height; uv: width*(height/2) (U,V pairs)
    void Resize(int w, int h);
};
// v210: 10-bit 4:2:2, 6 pixels in four little-endian 32-bit words (Cb Y Cr | Y Cb Y |
// Cr Y Cb | Y Cr Y); rows padded to 128 bytes. 4:2:2 -> 4:2:0 averages chroma pairs.
void V210ToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0);
void UyvyToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0);
void BgraToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0);
void CopyEye(P016& dst, int x0, int x1, int w);

}  // namespace mp::livepix
