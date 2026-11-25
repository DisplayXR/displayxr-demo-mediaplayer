// SPDX-License-Identifier: Apache-2.0
#include "MediaSource.h"
#include <cstring>

#include "Log.h"
#include "StereoDetect.h"

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

// Untagged media opens as 2D (mono). When neither the filename, the container metadata
// nor the content detector says a file is stereo, it is treated as an ordinary 2D
// picture: shown flat, and eligible for "Convert to 3D" through the 2D/3D switch.
//
// The player used to ASSUME stereo here and let the aspect settle full vs half SBS, so
// an ordinary 16:9 video with no tag (one the detector could not decide on, such as a
// dark, low-texture clip) showed squeezed and cropped as half-SBS, and the 2D/3D switch
// only flipped the panel mode instead of converting. An unlabelled real SBS file the
// detector cannot decide on now opens flat instead; `L` pins its layout, and a `*_2x1`
// name or a stereo tag still identifies it.

} // namespace

const char* LayoutName(StereoLayout l) {
    switch (l) {
        case StereoLayout::SbsFull: return "SBS-full";
        case StereoLayout::SbsHalf: return "SBS-half";
        case StereoLayout::Dual:    return "Dual";
        case StereoLayout::TbFull:  return "TB-full";
        case StereoLayout::TbHalf:  return "TB-half";
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
    // Top-bottom (#128): `*_1x2` / `*_half_1x2` mirror the SBS convention; the common
    // release tokens (TB, OU, TAB, H-OU, ...) are matched as whole words of the FILE
    // name, so "tabletop", "output" or a directory called tb never trip them. A bare
    // token is half (what those releases nearly always are) unless it says full.
    const size_t slash = lower.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? lower : lower.substr(slash + 1);
    if (Contains(base, "half_1x2")) { out = StereoLayout::TbHalf; return true; }
    if (Contains(base, "1x2")) { out = StereoLayout::TbFull; return true; }
    bool tb = false, full = false;
    std::string tok;
    for (size_t i = 0; i <= base.size(); ++i) {
        const char ch = i < base.size() ? base[i] : '.';
        if (std::isalnum((unsigned char)ch)) { tok += ch; continue; }
        if (tok == "tb" || tok == "ou" || tok == "tab" || tok == "htb" || tok == "hou") tb = true;
        if (tok == "ftb" || tok == "fou") tb = full = true;
        if (tok == "full") full = true;
        tok.clear();
    }
    if (tb) { out = full ? StereoLayout::TbFull : StereoLayout::TbHalf; return true; }
    return false;
}

namespace {
// Media Player Pro review formats (docs/PRO.md Next 6), off in the base player.
bool IsProReviewExt(const std::string& lower) {
    for (const char* e : {".mxf", ".exr", ".dpx", ".tif", ".tiff"})
        if (lower.size() >= std::strlen(e) && lower.compare(lower.size() - std::strlen(e), std::string::npos, e) == 0)
            return true;
    return false;
}
// HEIC / HEIF stills (#154), when the build can read them (MediaSource::SetHeifSupported).
bool IsHeifExt(const std::string& lower) {
    for (const char* e : {".heic", ".heif", ".hif"})
        if (lower.size() >= std::strlen(e) && lower.compare(lower.size() - std::strlen(e), std::string::npos, e) == 0)
            return true;
    return false;
}
}  // namespace

MediaInfo MediaSource::Identify(const std::string& path, int /*imageWidth*/, int /*imageHeight*/) {
    const std::string lower = Lower(path);
    MediaInfo info;

    if (EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg") || EndsWith(lower, ".png") ||
        EndsWith(lower, ".gif") || EndsWith(lower, ".bmp") ||
        EndsWith(lower, ".mpo") || EndsWith(lower, ".lif") || (heifSupported_ && IsHeifExt(lower))) {
        // .lif and .mpo are JPEG containers; their loaders compose the pair to SBS and
        // report the authoritative layout, so the layout decided below is unused for
        // those paths.
        info.kind = MediaKind::Image;
    } else if (EndsWith(lower, ".mp4") || EndsWith(lower, ".mkv") || EndsWith(lower, ".mov") ||
               EndsWith(lower, ".m4v") || EndsWith(lower, ".webm") || EndsWith(lower, ".3gp") ||
               (proFormats_ && IsProReviewExt(lower))) {
        info.kind = MediaKind::Video;  // (Pro: MXF, and EXR / DPX / TIFF played as a sequence)
    }

    // Stereo layout: filename suffix first (matches the `*_2x1` naming convention).
    if (LayoutFromFilename(path, info.layout)) {
        info.signal = StereoSignal::Filename;
        info.confidence = 1.0f;
    } else {
        // Untagged: 2D (see the note at the top of this file).
        info.layout = StereoLayout::Mono;
        info.signal = StereoSignal::Default;
        info.confidence = 1.0f;
    }

    LOG_INFO("MediaSource: '%s' -> kind=%s layout=%s (%s)", path.c_str(),
             KindName(info.kind), LayoutName(info.layout), SignalName(info.signal));
    return info;
}

MediaInfo MediaSource::Resolve(const std::string& path, MediaKind kind,
                               int /*frameW*/, int /*frameH*/,
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
    // 4. Content analysis, when it is switched on AND reached a verdict (stereo, or a
    //    confident mono). An undecided result falls through to the 2D default.
    } else if (content && content->decided) {
        info.layout = content->layout;
        info.signal = StereoSignal::Content;
        info.confidence = content->confidence;
    // 5. Nothing identified it: 2D (see the note at the top of this file).
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
           EndsWith(lower, ".gif") || EndsWith(lower, ".bmp") ||
           EndsWith(lower, ".lif") || EndsWith(lower, ".mpo") ||
           (heifSupported_ && IsHeifExt(lower)) ||
           EndsWith(lower, ".mp4") || EndsWith(lower, ".mkv") || EndsWith(lower, ".mov") ||
           EndsWith(lower, ".m4v") || EndsWith(lower, ".webm") || EndsWith(lower, ".3gp") ||
           (proFormats_ && IsProReviewExt(lower));
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
