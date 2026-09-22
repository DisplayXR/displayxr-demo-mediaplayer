// SPDX-License-Identifier: Apache-2.0
//
// Swapchain colour-encoding policy (#78).
//
// A UNORM colour swapchain *declares* linear pixels. Today's runtime lists UNORM first
// and passes the bytes through unchanged, so a player that hands it encoded (display-
// referred) video bytes in a UNORM image happens to look right. Once the runtime becomes
// format-honest (displayxr-runtime #1589 / #1606) it will take a UNORM swapchain at its
// word — decode nothing, then encode on compose — and every frame of this app would be
// double-encoded (washed out).
//
// So the app chooses for itself: prefer an `_SRGB` colour format and keep delivering the
// same encoded bytes. `_SRGB` says "these bytes are encoded", which is the truth.
//
// This mirrors displayxr-common's `common/color_policy.{h,cpp}` (DXR_SWAPCHAIN_ENCODING,
// default HonestSrgb). It is deliberately RE-TYPED, not linked: this demo is coupled to
// the runtime only by the OpenXR extension wire protocol (CLAUDE.md, golden rule), so it
// must not depend on a runtime-side library.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <vector>
#if !defined(_WIN32)
#include <strings.h>  // strcasecmp (POSIX; <cstring> does not declare it)
#endif

namespace mp {
namespace color {

// Vulkan format numbers, spelled out so this header needs no <vulkan/vulkan.h>
// (XrSession talks to the runtime in int64_t format numbers anyway).
constexpr int64_t kR8G8B8A8_UNORM = 37;
constexpr int64_t kR8G8B8A8_SRGB = 43;
constexpr int64_t kB8G8R8A8_UNORM = 44;
constexpr int64_t kB8G8R8A8_SRGB = 50;

enum class Encoding {
    HonestSrgb,  // default: prefer an _SRGB colour format
    Unorm,       // legacy: take the runtime's first format (today's behaviour)
};

// DXR_SWAPCHAIN_ENCODING=srgb|unorm. Anything else (unset, empty, unrecognised) =
// HonestSrgb. Read once per process.
inline Encoding EncodingFromEnv() {
    static const Encoding e = [] {
        const char* v = std::getenv("DXR_SWAPCHAIN_ENCODING");
        if (v && *v) {
#if defined(_WIN32)
            if (_stricmp(v, "unorm") == 0) return Encoding::Unorm;
#else
            if (strcasecmp(v, "unorm") == 0) return Encoding::Unorm;
#endif
        }
        return Encoding::HonestSrgb;
    }();
    return e;
}

inline const char* EncodingName(Encoding e) {
    return e == Encoding::Unorm ? "unorm" : "srgb";
}

inline bool IsSrgb8(int64_t format) {
    return format == kR8G8B8A8_SRGB || format == kB8G8R8A8_SRGB;
}

// The UNORM sibling of an _SRGB 8-bit format — same channel ORDER, same texel size, so
// vkCmdCopyImage between the two is a raw byte copy (VK "32-bit" compatibility class).
// That is how encoded bytes get into an _SRGB image without a converting blit.
inline int64_t UnormCounterpart(int64_t format) {
    if (format == kR8G8B8A8_SRGB) return kR8G8B8A8_UNORM;
    if (format == kB8G8R8A8_SRGB) return kB8G8R8A8_UNORM;
    return format;
}

// sRGB EOTF: a display-referred (encoded) channel value -> scene-linear.
// Anything written through an _SRGB image view is encoded by the hardware on store, so
// values handed to such a write (clear colours, shader output) must be linear first.
// Same function as displayxr-common's DisplayReferredToSceneLinear (runtime #1646).
inline float DisplayReferredToSceneLinear(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Pick the colour swapchain format.
//
//   HonestSrgb : the FIRST enumerated 8-bit _SRGB format. Scanning in the runtime's own
//                order keeps its channel-order preference (on desktop it lists BGRA
//                before RGBA because that is the swapchain-native order). Falls back to
//                formats[0] if the runtime enumerates no _SRGB format.
//   Unorm      : formats[0], i.e. exactly what this app did before #78.
//
// `why` receives a short static reason string for the one INFO line at creation.
inline int64_t ChooseColorFormat(const std::vector<int64_t>& formats, Encoding encoding,
                                 const char** why) {
    const char* reason = "legacy passthrough, exactly as before #78";
    int64_t chosen = formats.empty() ? 0 : formats[0];

    if (encoding == Encoding::HonestSrgb) {
        int64_t srgb = 0;
        for (int64_t f : formats) {
            if (IsSrgb8(f)) { srgb = f; break; }
        }
        if (srgb != 0) {
            chosen = srgb;
            reason = "encoding-honest: the bytes we deliver are display-referred";
        } else {
            reason = "no _SRGB format enumerated — falling back to the runtime's first";
        }
    }

    if (why) *why = reason;
    return chosen;
}

}  // namespace color
}  // namespace mp
