// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// DxrVideoAndroid — a DisplayXR Camera clip (`*_sbs.mp4`, the DisplayXR stereo media
// format v1, src/media/DxrStereo.h) as the Android leg opens it.
//
//   * the static half (layout sbs-lr, the clip's suggested convergence, which track is
//     the per-frame metadata) comes from the XMP `uuid` box, read by the format's
//     reference reader (dxrmedia, through dxrfile::ReadVideoFd) from the box tree only;
//   * the per-frame half is the `mett` track (MIME application/x-displayxr-stereo), read
//     once at open through AMediaExtractor with the extractor's own sample times — the
//     same move LvfProbeAndroid makes for a two-track file's convergence track. Playback
//     never touches the track again.
//
// Every value comes out as the player's per-eye shift (dxrfile::ShiftForDisparity, the one
// conversion). A file without the XMP box is not ours: `info.ours` is false and the
// caller opens it exactly as before.
#pragma once

#include "DxrStereo.h"

#include <cstdint>

namespace mp {

struct DxrVideoAndroid {
	struct Result {
		dxrfile::VideoInfo info;          // ours / sbs / static shift / metadata track id
		dxrfile::ConvergenceCurve curve;  // per-frame shifts, by presentation time (may be empty)
		int samplesRead = 0;          // samples the extractor handed out
		bool trackSeen = false;       // the extractor listed the metadata track at all
	};

	// `fd` is not consumed and its file offset is not used. A descriptor whose data does
	// not start at byte 0 (`offset` != 0) is not probed: the box tree is read from 0.
	static Result RunFd(int fd, int64_t offset, int64_t length);
};

}  // namespace mp
