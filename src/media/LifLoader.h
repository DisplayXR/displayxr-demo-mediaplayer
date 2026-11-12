// SPDX-License-Identifier: Apache-2.0
//
// LifLoader — parse a LIF (Leia Image Format) container: a normal JPEG with a
// metadata region appended at the end (trailer magic 0x1E1A; a u32 region offset
// at EOF-6; then a big-endian field table; the type-8/7 field is UTF-8 JSON; other
// fields are binary blobs — embedded view JPEGs and depth PNGs — keyed by blob_id).
//
// A two-view LIF already carries a baked left and right JPEG, so we extract both views
// and compose them into one full-SBS RGBA buffer that feeds the existing SBS render
// path unchanged. A one-view LIF with a disparity map gets its pair from DepthWarp
// (#180), on the CPU, so it takes the same path.
//
// Container layout reference: dfattal/LIF-renderer src/LifLoader.ts.
#pragma once

#include "ImageDecoder.h"   // DecodedImage
#include "StereoTypes.h"    // StereoLayout

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace mp {

struct LifResult {
    DecodedImage image;                        // composed SBS (stereo) or base JPEG (mono fallback)
    StereoLayout layout = StereoLayout::Mono;
    bool ok = false;                           // a renderable image was produced
    bool stereo = false;                       // two views were composed (or generated) side-by-side
    float convergence = 0.0f;                  // baked reconvergence from the LIF metadata
                                               // (normalized; 0 if absent/non-stereo)
    bool hasConvergence = false;               // the metadata actually carried a convergence field
    float autoConvergence = 0.0f;              // EstimateAutoConvergence of the pair, set only
                                               // when the field is missing; applied by default
};

class LifLoader {
public:
    // Cheap content sniff: true if the file ends with the LIF magic (0x1E1A). Real LIFs
    // commonly ship with a .jpg extension, so the dispatch must detect them by content,
    // not just by a .lif name. Reads only the last 2 bytes.
    static bool IsLif(const std::string& path);

    // The same sniff for a caller with no path (the media browser's 3D badge reads through
    // a file descriptor, #160): `readAt` reads `size` bytes at `offset`, false on failure.
    using ReadAtFn = std::function<bool(uint64_t offset, void* dst, size_t size)>;
    static bool IsLif(const ReadAtFn& readAt, uint64_t fileSize);

    // Where a two-view LIF's two view JPEGs are in the file, left then right by camera x,
    // read through `readAt` without loading the views: the trailer, the field headers and
    // the JSON only (a few KB of a 20 MB file). For the media browser's stereo thumbnails
    // (#180), which decode each view small. `base` = the view is the base JPEG (file
    // offset 0, up to the metadata region). False for anything else: no trailer, bad
    // JSON, fewer than two views (a one-view LIF's pair needs its depth map: Load()).
    struct ViewSpan {
        uint64_t offset = 0, size = 0;
        bool base = false;
    };
    static bool FindViews(const ReadAtFn& readAt, uint64_t fileSize, ViewSpan& left, ViewSpan& right);

    // Parse `path` and produce a renderable image:
    //   * 2+ views  -> compose left|right into a full-SBS RGBA buffer (SbsFull, stereo=true)
    //   * 1 view + disparity map -> generate left|right the same way (SbsFull, stereo=true)
    //   * otherwise -> decode the base JPEG as flat 2D (Mono) — never hard-fails on a
    //                  valid JPEG, even if the LIF trailer is missing or malformed.
    // Returns ok=false only if even the base image can't be decoded.
    static LifResult Load(const std::string& path);
};

} // namespace mp
