// SPDX-License-Identifier: Apache-2.0
#include "DxrStereo.h"

#include "dxrmedia/stills.h"
#include "dxrmedia/video.h"
#include "dxrmedia/xmp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mp::dxrfile {

namespace {

bool IsLayout(const std::string& v, const char* want) {
    if (v.size() != std::char_traits<char>::length(want)) return false;
    for (size_t i = 0; i < v.size(); ++i) {
        const char c = (v[i] >= 'A' && v[i] <= 'Z') ? (char)(v[i] - 'A' + 'a') : v[i];
        if (c != want[i]) return false;
    }
    return true;
}

// dxr:Convergence as the player's shift. A non-finite value is absent (FORMAT.md 0);
// dxrmedia already drops those, this only guards the float narrowing.
bool ShiftFrom(const dxrmedia::StereoMeta& m, float& out) {
    if (!m.convergence || !std::isfinite(*m.convergence)) return false;
    out = ShiftForDisparity((float)*m.convergence);
    return std::isfinite(out);
}

StillInfo FromStill(const dxrmedia::StillFile& f) {
    StillInfo s;
    if (f.status != dxrmedia::ReadStatus::Ours) return s;
    const dxrmedia::StereoMeta& m = f.meta.stereo;
    s.ours = true;
    s.pair = IsLayout(m.layout, "mpf-pair") && f.right() != nullptr;
    s.sbs = IsLayout(m.layout, "sbs-lr");
    s.hasConvergence = ShiftFrom(m, s.shift);
    s.eyeWidth = m.eyeWidth.value_or(0);
    s.eyeHeight = m.eyeHeight.value_or(0);
    return s;
}

VideoInfo FromProbe(const dxrmedia::Mp4Info& info) {
    VideoInfo v;
    if (!info.hasXmp) return v;
    dxrmedia::StereoMeta m;
    if (!dxrmedia::xmp_parse(info.xmpPacket, m)) return v;  // no dxr:Version: not ours
    v.ours = true;
    v.sbs = IsLayout(m.layout, "sbs-lr");
    v.hasConvergence = ShiftFrom(m, v.shift);
    if (info.metadataTrack >= 0 && (size_t)info.metadataTrack < info.tracks.size()) {
        v.metaTrackId = info.tracks[(size_t)info.metadataTrack].trackId;
        v.metaSamples = info.tracks[(size_t)info.metadataTrack].sampleCount;
    }
    return v;
}

}  // namespace

StillInfo ReadStill(const uint8_t* data, size_t size) {
    if (!data || size < 4) return {};
    return FromStill(dxrmedia::read_still(dxrmedia::Bytes(data, size)));
}

StillInfo ReadStillFile(const std::string& path) {
    return FromStill(dxrmedia::read_still_file(path));
}

VideoInfo ReadVideoFd(int fd) {
    if (fd < 0) return {};
    dxrmedia::Mp4Info info;
    if (!dxrmedia::mp4_probe_fd(fd, info)) return {};
    return FromProbe(info);
}

VideoInfo ReadVideo(const std::string& path) {
    dxrmedia::Mp4Info info;
    if (!dxrmedia::mp4_probe(path, info)) return {};
    return FromProbe(info);
}

bool ConvergenceCurve::Add(int64_t ptsUs, const uint8_t* sample, size_t size) {
    dxrmedia::FrameMeta f;
    if (!sample || !dxrmedia::decode_frame_meta(sample, size, f)) return false;
    if (!f.convergenceValid || !std::isfinite(f.convergence)) return false;
    const Point p{ptsUs, ShiftForDisparity(f.convergence), f.sceneCut};
    if (points_.empty() || ptsUs >= points_.back().ptsUs) {
        points_.push_back(p);
    } else {
        const auto at = std::upper_bound(points_.begin(), points_.end(), ptsUs,
                                         [](int64_t t, const Point& q) { return t < q.ptsUs; });
        points_.insert(at, p);
    }
    return true;
}

float ConvergenceCurve::ShiftAt(int64_t ptsUs, float fallback) const {
    if (points_.empty()) return fallback;
    if (ptsUs <= points_.front().ptsUs) return points_.front().shift;
    if (ptsUs >= points_.back().ptsUs) return points_.back().shift;
    const auto hi = std::upper_bound(points_.begin(), points_.end(), ptsUs,
                                     [](int64_t t, const Point& q) { return t < q.ptsUs; });
    const auto lo = hi - 1;
    // A cut at `hi` means `lo` and `hi` are different shots: hold `lo` until the cut.
    if (hi->cut) return lo->shift;
    const int64_t span = hi->ptsUs - lo->ptsUs;
    if (span <= 0) return lo->shift;
    const float t = (float)(ptsUs - lo->ptsUs) / (float)span;
    return lo->shift + (hi->shift - lo->shift) * t;
}

}  // namespace mp::dxrfile
