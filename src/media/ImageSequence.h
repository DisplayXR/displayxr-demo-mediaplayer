// SPDX-License-Identifier: Apache-2.0
//
// ImageSequence — numbered frame files (shot.1001.exr ... shot.1240.exr) as one clip,
// the way VFX review plays EXR / DPX / TIFF renders (Media Player Pro, phase 4). The
// decoder opens the sequence through FFmpeg's image2 demuxer with the printf pattern
// and start number found here. Pure; tests/image_sequence_test.cpp.
#pragma once

#include <string>

namespace mp::imageseq {

// True for the extensions played as sequences (.exr .dpx .tif .tiff).
bool IsSequenceExtension(const std::string& path);

struct Info {
    std::string pattern;  // printf pattern for FFmpeg, e.g. "C:/shots/a/shot.%04d.exr"
    int first = 0, last = 0, count = 0;  // frame numbers found on disk (count may be < last-first+1)
    int digits = 0;                       // zero-padded width
    std::string key;      // prefix + '#' + extension: one entry per sequence in a folder
};

// The sequence `path` belongs to. False when the name has no frame number (a single
// still), in which case the caller opens the file on its own.
bool Detect(const std::string& path, Info& out);

// The folder-listing key without touching the disk ("" when not a numbered frame).
std::string Key(const std::string& path);

}  // namespace mp::imageseq
