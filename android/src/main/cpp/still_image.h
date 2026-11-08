// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// StillImage: a picture as the player shows it. An image file goes through the shared
// loaders (a stereo LIF or MPO composed side by side, the LIF's convergence baked in), and
// the layout is resolved in the desktop's layers: the container, the filename, then the
// pixels. CPU only, so any thread: the player loads images with it and NeighbourStills
// decodes ahead with it.
#pragma once

#include "ImageDecoder.h"
#include "MediaSource.h"

#include <string>

namespace mp {

struct StillImage {
	DecodedImage image;
	MediaInfo info;
	bool convergenceBaked = false;

	// Copy `len` bytes of `fd` from `off` to the file at `path`: the loaders read paths.
	static bool Stage(int fd, long long off, long long len, const std::string &path);
	// The image file at `path`. `name` is the file name the layout rules read.
	static bool Decode(const std::string &path, const std::string &name, StillImage &out);
	// A video frame already in `image`: its layout from the filename, then the pixels.
	static void ResolveFrame(const std::string &name, StillImage &still);
};

}  // namespace mp
