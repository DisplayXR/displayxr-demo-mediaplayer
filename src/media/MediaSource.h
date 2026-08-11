// SPDX-License-Identifier: Apache-2.0
//
// MediaSource — identify a file's kind (image/video) and resolve its stereo layout.
//
// Layout resolution is LAYERED (#45): the first confident signal wins, and the winner is
// recorded in MediaInfo::signal so the HUD can say what decided it. In priority order:
//
//   1. manual   — the user pinned it (L key / HUD button)
//   2. metadata — AVStereo3D side data, or a LIF / MPO container
//   3. filename — the `*_2x1` / `*_half_2x1` naming convention
//   4. content  — StereoDetect's cross-correlation verdict. OPT-IN, off by default;
//                 see MEDIAPLAYER_STEREO_DETECT.
//   5. aspect   — assume SBS and let the frame aspect settle full-vs-half.
//
// The important thing about layer 5 is that it does NOT try to decide whether a file is
// stereo. This is a stereo player for a 3D display; mono is not a use case. So an
// unidentified file is ASSUMED stereo and the aspect only picks the packing.
//
// That assumption is what finally fixes the case a threshold never could: a half-SBS
// frame is DIMENSIONALLY IDENTICAL to a mono one (1920x1080 either way). The old rule
// (`aspect >= 1.9 -> full SBS, else mono`) therefore showed real half-SBS files flat,
// and split 2:1 mono panoramas — wrong in both directions. Assuming stereo is wrong only
// for mono content, which this player does not target.
#pragma once

#include "StereoTypes.h"

#include <string>

namespace mp {

enum class MediaKind { Unknown, Image, Video };

struct MediaInfo {
    MediaKind kind = MediaKind::Unknown;
    StereoLayout layout = StereoLayout::Mono;
    StereoSignal signal = StereoSignal::Default;  // which layer decided `layout`
    bool eyeSwap = false;                         // packing is R|L (metadata layer only)
    float confidence = 0.0f;                      // 0..1
};

class MediaSource {
public:
    // Identify from filename; for images, pass decoded dims so an unsuffixed but
    // clearly-wide (>=1.9:1) frame is treated as full SBS.
    //
    // Behaviour is UNCHANGED from before the layered detector landed, and a regression
    // test locks that. It stays callable before anything is decoded, because App picks
    // its load branch off `kind` while layout needs pixels that only exist afterwards --
    // which is why the layered verdict lives in Resolve() rather than in an overload here.
    static MediaInfo Identify(const std::string& path, int imageWidth = 0, int imageHeight = 0);

    // Layer 3 in isolation. Returns false if the filename says nothing.
    static bool LayoutFromFilename(const std::string& path, StereoLayout& out);

    // The layered resolution. Every pointer is optional -- nullptr means "that layer had
    // nothing to say". `content` is consulted only when it is `decided`.
    static MediaInfo Resolve(const std::string& path, MediaKind kind,
                             int frameW, int frameH,
                             const StereoLayout* manual,
                             const MediaInfo* meta,
                             const StereoDetectResult* content);

    // True if the extension is a supported image or video (used to filter a folder for
    // prev/next navigation, and to filter dropped files). The single source of truth for
    // the supported set.
    static bool IsSupported(const std::string& path);

    static const char* KindName(MediaKind k);
    static const char* LayoutName(StereoLayout l);   // forwards to mp::LayoutName
    static const char* SignalName(StereoSignal s);   // forwards to mp::SignalName
};

} // namespace mp
