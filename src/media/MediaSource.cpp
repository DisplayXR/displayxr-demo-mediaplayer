// SPDX-License-Identifier: BSL-1.0
#include "MediaSource.h"

#include "Log.h"

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

} // namespace

MediaInfo MediaSource::Identify(const std::string& path, int imageWidth, int imageHeight) {
    const std::string lower = Lower(path);
    MediaInfo info;

    if (EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg") || EndsWith(lower, ".png")) {
        info.kind = MediaKind::Image;
    } else if (EndsWith(lower, ".mp4") || EndsWith(lower, ".mkv") || EndsWith(lower, ".mov")) {
        info.kind = MediaKind::Video;
    }

    // Stereo layout: filename suffix first (matches Leia's convention).
    if (Contains(lower, "half_2x1")) {
        info.layout = StereoLayout::SbsHalf;
    } else if (Contains(lower, "2x1")) {
        info.layout = StereoLayout::SbsFull;
    } else if (imageWidth > 0 && imageHeight > 0 &&
               (float)imageWidth / (float)imageHeight >= 1.9f) {
        // No suffix but unusually wide -> assume full SBS.
        info.layout = StereoLayout::SbsFull;
    } else {
        info.layout = StereoLayout::Mono;
    }

    LOG_INFO("MediaSource: '%s' -> kind=%s layout=%s", path.c_str(),
             KindName(info.kind), LayoutName(info.layout));
    return info;
}

bool MediaSource::IsSupported(const std::string& path) {
    const std::string lower = Lower(path);
    return EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg") || EndsWith(lower, ".png") ||
           EndsWith(lower, ".mp4") || EndsWith(lower, ".mkv") || EndsWith(lower, ".mov");
}

const char* MediaSource::KindName(MediaKind k) {
    switch (k) {
        case MediaKind::Image: return "image";
        case MediaKind::Video: return "video";
        default: return "unknown";
    }
}

const char* MediaSource::LayoutName(StereoLayout l) {
    switch (l) {
        case StereoLayout::SbsFull: return "SBS-full";
        case StereoLayout::SbsHalf: return "SBS-half";
        default: return "mono";
    }
}

} // namespace mp
