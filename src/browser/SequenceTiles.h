// SPDX-License-Identifier: Apache-2.0
//
// SequenceTiles (#267): the browser shows a render's numbered EXR / DPX / TIFF frames as
// one tile per sequence (imageseq::Key), named with its frame range, instead of a tile per
// frame. The tile opens its lowest frame, which the player opens as the whole sequence.
// Pure (no disk access); tests/pro_browser_test.cpp.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mp::browser {

struct SequenceTile {
    size_t first = 0;             // index (into the paths given) of the lowest frame
    std::vector<size_t> members;  // every frame's index, `first` included
    int firstFrame = 0, lastFrame = 0;
};

// The sequences among `paths` (two frames or more with the same imageseq::Key). A lone
// numbered frame and every other file are left alone (not in the result).
std::vector<SequenceTile> GroupSequences(const std::vector<std::string>& paths);

// The tile's name from its lowest frame's file name: "shot.1001.exr" -> "shot.[1001-1240].exr".
// It keeps the extension (the browser decides what it can open by the name) and has no '#'
// (ImGui reads "##" in a label as an id). A name without a frame number comes back as it is.
std::string SequenceTileName(const std::string& firstName, int firstFrame, int lastFrame);

}  // namespace mp::browser
