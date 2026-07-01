// SPDX-License-Identifier: Apache-2.0
//
// MediaSource (M1) — identify a file's kind (image/video) and stereo layout from
// its name (Leia convention: `*_2x1` full SBS, `*_half_2x1` half-width SBS), with
// an aspect-ratio fallback for images that carry no suffix. Video routing is M2.
#pragma once

#include <string>

namespace mp {

enum class StereoLayout {
    Mono,     // 2D — same image to both eyes
    SbsFull,  // full side-by-side: each eye is half the pixel width
    SbsHalf,  // half side-by-side: each eye is squeezed; stretched on display
};

enum class MediaKind { Unknown, Image, Video };

struct MediaInfo {
    MediaKind kind = MediaKind::Unknown;
    StereoLayout layout = StereoLayout::Mono;
};

class MediaSource {
public:
    // Identify from filename; for images, pass decoded dims so an unsuffixed but
    // clearly-wide (>=1.9:1) frame is treated as full SBS.
    static MediaInfo Identify(const std::string& path, int imageWidth = 0, int imageHeight = 0);

    // True if the extension is a supported image or video (used to filter a folder for
    // prev/next navigation). The single source of truth for the supported set.
    static bool IsSupported(const std::string& path);

    static const char* KindName(MediaKind k);
    static const char* LayoutName(StereoLayout l);
};

} // namespace mp
