// SPDX-License-Identifier: Apache-2.0
#include "MediaSource.h"

#include "Log.h"
#include "StereoDetect.h"   // ChooseFullOrHalf — pure aspect policy, no pixels involved

#include <algorithm>
#include <cctype>

namespace mp {

namespace {

std::string Lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const std::string suf = suffix;
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool Contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

// Last resort, and the deliberate default policy of this player.
//
// This is a STEREO media player for a 3D display. Viewing mono content is not a use
// case, so when nothing has identified a file the working assumption is that it IS
// stereo, and the frame aspect only has to settle full-vs-half. That is a policy call,
// not a measurement -- and it is what finally fixes the case the old rule could not:
// an unsuffixed 1920x1080 half-SBS frame is dimensionally identical to a mono one, so
// no aspect THRESHOLD can separate them, but under this assumption it simply resolves
// to half-SBS.
//
// The old rule was `aspect >= 1.9 -> full SBS, else mono`, which got the common
// half-SBS case wrong in the direction that hurts: a real stereo file shown flat.
//
// Cost, accepted knowingly: a genuinely mono file with no suffix and no metadata --
// e.g. an ordinary photo reached by arrow-keying through a folder -- renders as a
// squeezed half-SBS. `L` is the escape hatch, and the content detector
// (MEDIAPLAYER_STEREO_DETECT=full) can be switched on to catch it automatically.
bool LayoutFromAspect(int w, int h, StereoLayout& out) {
    if (w <= 0 || h <= 0) return false;   // no dimensions: nothing to go on at all
    bool ambiguous = false;
    out = StereoDetect::ChooseFullOrHalf((float)w / (float)h, ambiguous);
    return true;
}

} // namespace

const char* LayoutName(StereoLayout l) {
    switch (l) {
        case StereoLayout::SbsFull: return "SBS-full";
        case StereoLayout::SbsHalf: return "SBS-half";
        default: return "mono";
    }
}

const char* SignalName(StereoSignal s) {
    switch (s) {
        case StereoSignal::Manual: return "manual";
        case StereoSignal::Metadata: return "metadata";
        case StereoSignal::Filename: return "from filename";
        case StereoSignal::Content: return "detected";
        case StereoSignal::Aspect: return "assumed stereo (aspect)";
        default: return "default";
    }
}

bool MediaSource::LayoutFromFilename(const std::string& path, StereoLayout& out) {
    const std::string lower = Lower(path);
    if (Contains(lower, "half_2x1")) { out = StereoLayout::SbsHalf; return true; }
    if (Contains(lower, "2x1")) { out = StereoLayout::SbsFull; return true; }
    return false;
}

MediaInfo MediaSource::Identify(const std::string& path, int imageWidth, int imageHeight) {
    const std::string lower = Lower(path);
    MediaInfo info;

    if (EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg") || EndsWith(lower, ".png") ||
        EndsWith(lower, ".mpo") || EndsWith(lower, ".lif")) {
        // .lif and .mpo are JPEG containers; their loaders compose the pair to SBS and
        // report the authoritative layout, so the layout decided below is unused for
        // those paths.
        info.kind = MediaKind::Image;
    } else if (EndsWith(lower, ".mp4") || EndsWith(lower, ".mkv") || EndsWith(lower, ".mov")) {
        info.kind = MediaKind::Video;
    }

    // Stereo layout: filename suffix first (matches the `*_2x1` naming convention).
    if (LayoutFromFilename(path, info.layout)) {
        info.signal = StereoSignal::Filename;
        info.confidence = 1.0f;
    } else if (LayoutFromAspect(imageWidth, imageHeight, info.layout)) {
        info.signal = StereoSignal::Aspect;
        info.confidence = 0.3f;
    } else {
        info.layout = StereoLayout::Mono;
        info.signal = StereoSignal::Default;
        info.confidence = 1.0f;
    }

    LOG_INFO("MediaSource: '%s' -> kind=%s layout=%s (%s)", path.c_str(),
             KindName(info.kind), LayoutName(info.layout), SignalName(info.signal));
    return info;
}

MediaInfo MediaSource::Resolve(const std::string& path, MediaKind kind,
                               int frameW, int frameH,
                               const StereoLayout* manual,
                               const MediaInfo* meta,
                               const StereoDetectResult* content) {
    MediaInfo info;
    info.kind = kind;

    // 1. Manual pin beats everything — no heuristic is perfect, and the user is looking
    //    at the result.
    if (manual) {
        info.layout = *manual;
        info.signal = StereoSignal::Manual;
        info.confidence = 1.0f;
    // 2. Filename convention. ABOVE container metadata, deliberately: the `*_2x1` suffix
    //    is an explicit label applied by whoever produced the file, and it is the rule
    //    every existing asset already relies on. Ranking a container tag over it would
    //    change how files that work today are rendered -- a `*_2x1` clip carrying a
    //    top-bottom or 2D AVStereo3D tag would start showing as mono. Keeping the name on
    //    top makes "existing `*_2x1` assets are unaffected" a guarantee rather than a
    //    likelihood.
    } else if (LayoutFromFilename(path, info.layout)) {
        info.signal = StereoSignal::Filename;
        info.confidence = 1.0f;
        // Eye ORDER still comes from the tag when there is one: the filename convention
        // says how the frame is packed, never which half is the left eye. This can only
        // correct a file that was previously rendered with the eyes swapped.
        if (meta) info.eyeSwap = meta->eyeSwap;
    // 3. Container metadata.
    } else if (meta) {
        info.layout = meta->layout;
        info.eyeSwap = meta->eyeSwap;
        info.signal = StereoSignal::Metadata;
        info.confidence = 1.0f;
    // 4. Content analysis, when it is switched on AND reached a verdict. A CONFIDENT
    //    MONO is a real verdict and wins here — that is the only way a mono file gets
    //    recognised as such, since layer 5 assumes stereo by policy.
    } else if (content && content->decided) {
        info.layout = content->layout;
        info.signal = StereoSignal::Content;
        info.confidence = content->confidence;
    // 5. Assume stereo; aspect only settles full vs half.
    } else if (LayoutFromAspect(frameW, frameH, info.layout)) {
        info.signal = StereoSignal::Aspect;
        info.confidence = 0.3f;
    } else {
        info.layout = StereoLayout::Mono;
        info.signal = StereoSignal::Default;
        info.confidence = 1.0f;
    }

    LOG_INFO("MediaSource: '%s' -> layout=%s signal=%s conf=%.2f%s", path.c_str(),
             LayoutName(info.layout), SignalName(info.signal), (double)info.confidence,
             info.eyeSwap ? " eyes=R|L" : "");
    return info;
}

bool MediaSource::IsSupported(const std::string& path) {
    const std::string lower = Lower(path);
    return EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg") || EndsWith(lower, ".png") ||
           EndsWith(lower, ".lif") || EndsWith(lower, ".mpo") ||
           EndsWith(lower, ".mp4") || EndsWith(lower, ".mkv") || EndsWith(lower, ".mov");
}

const char* MediaSource::KindName(MediaKind k) {
    switch (k) {
        case MediaKind::Image: return "image";
        case MediaKind::Video: return "video";
        default: return "unknown";
    }
}

const char* MediaSource::LayoutName(StereoLayout l) { return mp::LayoutName(l); }
const char* MediaSource::SignalName(StereoSignal s) { return mp::SignalName(s); }

} // namespace mp
