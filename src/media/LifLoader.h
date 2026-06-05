// SPDX-License-Identifier: BSL-1.0
//
// LifLoader — parse a LIF (Leia Image Format) container: a normal JPEG with a
// metadata region appended at the end (trailer magic 0x1E1A; a u32 region offset
// at EOF-6; then a big-endian field table; the type-8/7 field is UTF-8 JSON; other
// fields are binary blobs — embedded view JPEGs and depth PNGs — keyed by blob_id).
//
// Step 1 scope: STEREO LIFs only. A two-view LIF already carries a baked left and
// right JPEG, so we extract both views and compose them into one full-SBS RGBA
// buffer that feeds the existing SBS render path unchanged — no depth warp, no new
// shaders. (Mono+depth synthesis / the raycast shader port is a later step.)
//
// Container layout reference: dfattal/LIF-renderer src/LifLoader.ts.
#pragma once

#include "ImageDecoder.h"   // DecodedImage
#include "MediaSource.h"    // StereoLayout

#include <string>

namespace mp {

struct LifResult {
    DecodedImage image;                        // composed SBS (stereo) or base JPEG (mono fallback)
    StereoLayout layout = StereoLayout::Mono;
    bool ok = false;                           // a renderable image was produced
    bool stereo = false;                       // two views were composed side-by-side
};

class LifLoader {
public:
    // Parse `path` and produce a renderable image:
    //   * 2+ views  -> compose left|right into a full-SBS RGBA buffer (SbsFull, stereo=true)
    //   * otherwise -> decode the base JPEG as flat 2D (Mono) — never hard-fails on a
    //                  valid JPEG, even if the LIF trailer is missing or malformed.
    // Returns ok=false only if even the base image can't be decoded.
    static LifResult Load(const std::string& path);
};

} // namespace mp
