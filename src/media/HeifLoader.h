// SPDX-License-Identifier: Apache-2.0
//
// HeifLoader (#154) — stereo HEIC photos (Apple spatial photos, Samsung Galaxy "3D Capture"
// photos) and ordinary HEIC stills.
//
// A stereo HEIC stores each eye as its own image item and ties the two together with a
// HEIF `ster` entity group (ISO/IEC 23008-12): entity 0 is the left view, entity 1 the
// right one. Like MpoLoader / LifLoader this loader composes the pair into one full-SBS
// RGBA buffer, so it feeds the existing SBS path unchanged (auto-convergence included).
// A HEIC without a `ster` group shows its primary item (`pitm`) flat.
//
// Decoding goes through the FFmpeg we already ship rather than a new dependency: FFmpeg's
// mov demuxer (7.1+) exposes every HEIF image item as a stream whose id is the item id, and
// every grid item (phones store photos as 512x512 tiles) as a TILE_GRID stream group whose
// id is the grid's item id. FFmpeg does NOT expose the `ster` group, so its two entity ids
// come from SpatialMedia's own bounded box walk and are matched to those ids here.
// (libheif was the alternative; it would have meant a fourth native library to build and
// bundle on Windows, macOS and the Linux .deb for one container format.)
//
// Header stays libav-free (VideoDecoder's discipline).
#pragma once

#include "ImageDecoder.h"   // DecodedImage
#include "StereoTypes.h"    // StereoLayout

#include <string>

namespace mp {

struct HeifResult {
    DecodedImage image;                        // composed SBS when stereo, else the primary image
    StereoLayout layout = StereoLayout::Mono;  // SbsFull when stereo
    bool ok = false;                           // a picture was produced
    bool stereo = false;                       // the `ster` pair was found AND both eyes decoded
    float autoConvergence = 0.0f;              // coarse estimate (stereo only), as for MPO
};

class HeifLoader {
public:
    // This build's FFmpeg can demux HEIF items (FFmpeg 7.1+). When false, .heic is not
    // offered at all (MediaSource::SetHeifSupported).
    static bool Available();

    // .heic / .heif / .hif by extension.
    static bool IsHeifPath(const std::string& path);

    // Decode `path`: the stereo pair composed left|right, or the primary image alone.
    static HeifResult Load(const std::string& path);
};

} // namespace mp
