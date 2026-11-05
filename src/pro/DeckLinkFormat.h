// SPDX-License-Identifier: Apache-2.0
//
// DeckLinkFormat — which pixel format to ask a DeckLink input for, in order, and what
// that costs in bit depth (#169). Pure: no SDK types, so it is unit-tested
// (tests/live_pixels_test.cpp); DeckLinkSource.cpp maps Pix to BMDPixelFormat.
//
// Every format here has a converter to P016 in LivePixels:
//   Yuv10 v210 (bmdFormat10BitYUV)   Yuv8  2vuy (bmdFormat8BitYUV)
//   Rgb10 r210 (bmdFormat10BitRGB)   Bgra8 BGRA (bmdFormat8BitBGRA)
// 12-bit RGB (R12B / R12L) is not requested: it has no converter yet, so a 12-bit RGB
// signal is captured as r210 and the 2 bits lost are warned about (DepthLossText).
#pragma once

#include <string>

namespace mp::dlfmt {

enum class Pix { Yuv10, Yuv8, Rgb10, Bgra8 };

inline int Bits(Pix p) { return (p == Pix::Yuv10 || p == Pix::Rgb10) ? 10 : 8; }
inline bool IsRgb(Pix p) { return p == Pix::Rgb10 || p == Pix::Bgra8; }
inline const char* Name(Pix p) {
    switch (p) {
        case Pix::Yuv10: return "10-bit YUV 4:2:2 (v210)";
        case Pix::Yuv8: return "8-bit YUV 4:2:2";
        case Pix::Rgb10: return "10-bit RGB (r210)";
        case Pix::Bgra8: return "8-bit RGB (BGRA)";
    }
    return "?";
}

// The formats to try for a signal, best first; returns how many were written to out[4].
// rgb: the card detected RGB 4:4:4. signalBits: 8 / 10 / 12 from the card's detection,
// 0 when it did not say. yuv10Ok: false once the card refused 10-bit YUV at open.
// An RGB signal tries RGB first (10-bit unless the signal is known to be 8-bit), then
// YUV, which the card converts to, so a refused RGB format still gives a picture.
inline int Candidates(bool rgb, int signalBits, bool yuv10Ok, Pix out[4]) {
    int n = 0;
    if (rgb) {
        if (signalBits != 8) out[n++] = Pix::Rgb10;  // r210 also holds an 8-bit signal exactly
        out[n++] = Pix::Bgra8;
    }
    if (yuv10Ok) out[n++] = Pix::Yuv10;
    out[n++] = Pix::Yuv8;
    return n;
}

// "" when `captured` keeps the signal's depth (or the depth is unknown), else a warning
// for the log and the toast, e.g. "12-bit RGB signal captured at 10 bits".
inline std::string DepthLossText(bool rgb, int signalBits, Pix captured) {
    if (signalBits <= 0 || Bits(captured) >= signalBits) return {};
    return std::to_string(signalBits) + "-bit " + (rgb ? "RGB" : "YUV") + " signal captured as " + Name(captured) +
           ": " + std::to_string(signalBits - Bits(captured)) + " bits of depth lost";
}

}  // namespace mp::dlfmt
