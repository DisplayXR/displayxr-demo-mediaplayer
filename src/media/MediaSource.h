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
//   4. content  — StereoDetect's cross-correlation verdict. It runs ONLY when 1-3 all
//                 declined, and may conclude MONO only on unambiguous evidence.
//   5. default  — 2D (mono). Nothing above identified it, so it is an ordinary picture:
//                 shown flat, and the 2D/3D switch converts it.
//
// Untagged media used to be ASSUMED stereo at layer 5, with the aspect settling full vs
// half SBS. That showed every 16:9 video the detector could not decide on (a dark or
// low-texture clip) squeezed as half-SBS, and its 2D/3D switch flipped the panel mode
// instead of converting. The cost of defaulting to 2D: an unlabelled real SBS file the
// detector finds ambiguous opens flat until the user pins its layout with L.
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
    // The container format when it IS the stereo signal and the user would not recognise
    // the packing name (#154): "MV-HEVC" / "stereo HEIC" compose to SbsFull internally, so
    // the HUD says the format instead. Static string; "" for everything else.
    const char* format = "";
};

class MediaSource {
public:
    // Identify from the filename: the kind, and the layout a `*_2x1` / top-bottom name
    // gives; anything else is 2D. The dims are no longer used (untagged media is 2D,
    // whatever its aspect) and stay only so existing callers compile.
    //
    // It stays callable before anything is decoded, because App picks
    // its load branch off `kind` while layout needs pixels that only exist afterwards --
    // which is why the layered verdict lives in Resolve() rather than in an overload here.
    static MediaInfo Identify(const std::string& path, int imageWidth = 0, int imageHeight = 0);

    // Layer 3 in isolation. Returns false if the filename says nothing.
    static bool LayoutFromFilename(const std::string& path, StereoLayout& out);

    // The layered resolution. Every pointer is optional -- nullptr means "that layer had
    // nothing to say". `content` is consulted only when it is `decided`. When no layer
    // decides, the result is 2D (Mono); the frame size no longer matters.
    static MediaInfo Resolve(const std::string& path, MediaKind kind,
                             int frameW, int frameH,
                             const StereoLayout* manual,
                             const MediaInfo* meta,
                             const StereoDetectResult* content);

    // True if the extension is a supported image or video (used to filter a folder for
    // prev/next navigation, and to filter dropped files). The single source of truth for
    // the supported set.
    static bool IsSupported(const std::string& path);
    // Media Player Pro review formats (MXF, EXR / DPX / TIFF sequences); off in the base
    // player. Pro turns it on (with VideoDecoder::SetProFormats) before the first load.
    static void SetProFormats(bool on) { proFormats_ = on; }
    static inline bool proFormats_ = false;
    // HEIC / HEIF stills (#154): offered only where HeifLoader can read them (a desktop build
    // on FFmpeg 7.1+). Off by default, so the Android leg — which shares this file but not
    // the loader — never lists a .heic it cannot open.
    static void SetHeifSupported(bool on) { heifSupported_ = on; }
    static inline bool heifSupported_ = false;

    static const char* KindName(MediaKind k);
    static const char* LayoutName(StereoLayout l);   // forwards to mp::LayoutName
    static const char* SignalName(StereoSignal s);   // forwards to mp::SignalName
};

} // namespace mp
