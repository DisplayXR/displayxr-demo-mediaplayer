// SPDX-License-Identifier: Apache-2.0
//
// Pixel helpers for live capture sources: an SDI/HDMI card delivers 4:2:2 YUV (UYVY,
// v210) or RGB (BGRA, r210); the renderer takes NV12 / P016. Each eye is converted straight
// into its place in a side-by-side NV12 frame, so a two-input pair costs one pass and
// no extra copy. Pure functions, unit-tested (tests/live_pixels_test.cpp).
#pragma once

#include "media/FrameRing.h"  // ColorMatrix

#include <cstdint>
#include <vector>

namespace mp::livepix {

// A tight NV12 frame: Y rows (width bytes each), then interleaved UV rows (width bytes,
// height/2 rows). width and height are even.
struct Nv12 {
    int width = 0, height = 0;
    std::vector<uint8_t> y, uv;
    // The Y'CbCr matrix of the samples - publish it as FrameRing::Frame::matrix. The
    // converters below set it (BGRA: the matrix they encode with; UYVY: the SDI rule, SD =
    // BT.601, HD = BT.709); a source that knows better (the card reports the signal's
    // colorimetry) overwrites it after converting.
    ColorMatrix matrix = ColorMatrix::BT709;
    ColorTransfer transfer = ColorTransfer::SDR;  // tag only (FrameRing::Frame::transfer)
    void Resize(int w, int h);
};

// Convert one eye (w x h) into dst at column x0 (even). UYVY: Cb Y0 Cr Y1 per 2 pixels.
// 4:2:2 -> 4:2:0 averages each vertical chroma pair.
void UyvyToNv12(const uint8_t* src, int srcRowBytes, int w, int h, Nv12& dst, int x0);
// BGRA (an RGB 4:4:4 signal) -> limited-range BT.709 NV12 (BT.601 below 720 lines; dst.matrix
// says which).
void BgraToNv12(const uint8_t* src, int srcRowBytes, int w, int h, Nv12& dst, int x0);
// Copy one eye of dst (columns [x0, x0+w)) onto columns [x1, x1+w) (repeat an eye).
void CopyEye(Nv12& dst, int x0, int x1, int w);

// The 16-bit twin (FrameRing P016): NV12 layout, samples MSB-aligned in 16 bits. The
// DeckLink path captures 10-bit and keeps it to the GPU, so the look grades at 10 bits.
struct P016 {
    int width = 0, height = 0;
    std::vector<uint16_t> y, uv;  // y: width*height; uv: width*(height/2) (U,V pairs)
    ColorMatrix matrix = ColorMatrix::BT709;  // as Nv12::matrix
    ColorTransfer transfer = ColorTransfer::SDR;
    void Resize(int w, int h);
};
// v210: 10-bit 4:2:2, 6 pixels in four little-endian 32-bit words (Cb Y Cr | Y Cb Y |
// Cr Y Cb | Y Cr Y); rows padded to 128 bytes. 4:2:2 -> 4:2:0 averages chroma pairs.
void V210ToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0);
void UyvyToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0);
void BgraToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0);
// r210 (DeckLink bmdFormat10BitRGB, an RGB 4:4:4 signal at 10 bits): one big-endian 32-bit
// word per pixel, 2 unused bits then R G B at 10 bits each, SMPTE levels (64-940). Encoded to
// limited-range Y'CbCr with `matrix` (dst.matrix is set to it), so the 10 bits reach the
// grade; 4:4:4 -> 4:2:0 averages each 2x2 block's RGB.
void R210ToP016(const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0, ColorMatrix matrix);
void CopyEye(P016& dst, int x0, int x1, int w);

// ---- The four P016 converters above as one, and in row bands (a 4K60 eye split across
// threads: pro/ConvertPool.h). Each converter is ToP016 with its format; ToP016 is the
// tag plus ToP016Rows over [0, h).
enum class RawFormat { Uyvy, V210, Bgra, R210 };

// One eye -> P016, as the converter for `fmt` (rgbMatrix: R210's encode matrix, else unused).
void ToP016(RawFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0,
            ColorMatrix rgbMatrix);
// The matrix ToP016 tags dst with: UYVY / v210 the SDI rule (DefaultMatrixForHeight), BGRA
// its own encode's (BT.709 from 720 lines), r210 rgbMatrix.
ColorMatrix P016Matrix(RawFormat fmt, int h, ColorMatrix rgbMatrix);
// Whether the converters take this eye (they write nothing otherwise).
bool EyeFits(const uint8_t* src, int w, int h, const P016& dst, int x0);
// Rows [r0, r1) of the eye only: luma rows r0..r1-1 and chroma rows r0/2..r1/2-1 of columns
// [x0, x0 + w), never dst's tags. r0 must be even (a 4:2:0 chroma row is made from rows 2k
// and 2k + 1) and r1 even or h; false (nothing written) otherwise or when the eye does not
// fit. Bands that tile [0, h) write disjoint samples, so they can run on separate threads,
// and together they write what ToP016 writes, bit for bit: it is this over [0, h).
bool ToP016Rows(RawFormat fmt, const uint8_t* src, int srcRowBytes, int w, int h, P016& dst, int x0, int r0,
                int r1, ColorMatrix rgbMatrix);

}  // namespace mp::livepix
