// SPDX-License-Identifier: Apache-2.0
//
// VideoThumbnail: one still from a local video, for the browser's grid when the system
// has none (#252). Windows makes no thumbnail for many videos (HEVC without its codec
// pack, MKV, large files), so the desktop browser falls back to this. Software decode,
// a few frames at most, a frame some way in (the first is often black). Never call it on
// a cloud placeholder: it reads the file.
//
// Header stays libav-free.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mp {

// Fills `rgba` (width x height, fitting in maxEdge x maxEdge) and returns true, or false if
// the file did not decode (or the build has no FFmpeg). Gives up after about `budgetMs`.
bool VideoThumbnail(const std::string& path, uint32_t maxEdge, uint32_t& width, uint32_t& height,
                    std::vector<uint8_t>& rgba, int budgetMs = 1500);

} // namespace mp
