// SPDX-License-Identifier: Apache-2.0
//
// DxrVideoTrack — the desktop demux of a DisplayXR Camera clip's per-frame convergence
// (FORMAT.md 3.3: the `mett` track, MIME application/x-displayxr-stereo, one 32-byte
// sample per video frame at the frame's own timestamp).
//
// FFmpeg's mov demuxer exposes that track as a DATA stream whose AVStream::id is the
// track's tkhd id, which DxrStereo's box probe (dxrfile::ReadVideo) names. This opens its own
// short-lived context, discards every other stream (the demuxer then skips their
// samples without reading them) and reads the whole track once, at open: a clip has one
// tiny sample per frame, and playback never touches this stream again. The Android leg
// does the same through AMediaExtractor (android/src/main/cpp/dxr_video_android.cpp).
//
// Header stays libav-free.
#pragma once

#include "DxrStereo.h"

#include <cstdint>
#include <string>

namespace mp::dxrfile {

// Fill `curve` from track `trackId` of `path`. Returns the number of samples read (0 when
// the stream is missing or FFmpeg is not built in); `curve` keeps only the valid ones.
int ReadConvergenceTrack(const std::string& path, uint32_t trackId, ConvergenceCurve& curve);

}  // namespace mp::dxrfile
