// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// VideoStereoProbeAndroid — the Android leg of the desktop's VideoStereoProbe
// (src/media/VideoStereoProbe.h): decode the first few frames of a clip on the
// CPU and let StereoDetect vote on how they are packed (mono / full SBS / half
// SBS). Needed because playback here is zero-copy -- the decoder writes vendor
// YUV straight into GPU-only AHardwareBuffers, so there is never a CPU-readable
// frame to analyse. A second, short-lived AMediaCodec configured WITHOUT a
// surface (YUV420Flexible output) gives us luma for the ~7 frames the vote
// needs, then goes away. Synchronous at open, like the desktop probe, and for
// the same reason: the first frame shown must already be split correctly.
#pragma once

#include "StereoDetect.h"

#include <cstdint>

namespace mp {

struct VideoStereoProbeAndroid {
	struct Result {
		bool ok = false;               // opened, had a video track, decoded >= 1 frame
		int width = 0, height = 0;     // coded frame size
		StereoDetectResult content{};  // the vote; content.decided says whether it counts
		int votes = 0;
		int framesExamined = 0;
		int64_t elapsedUs = 0;
	};
	// Probe by fd (SAF picker; the fd is dup'ed internally, caller keeps ownership)
	// or by path (debug auto-open).
	static Result RunFd(int fd, int64_t offset, int64_t length);
	static Result RunPath(const char *path);
};

}  // namespace mp
