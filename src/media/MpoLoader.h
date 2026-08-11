// SPDX-License-Identifier: Apache-2.0
//
// MpoLoader — parse an MPO (Multi-Picture Object): several complete JPEGs concatenated
// into one file, indexed by an APP2 segment whose payload starts "MPF\0" (#45).
//
// MPO is a CONTAINER, not a packing layout, so this mirrors LifLoader: it extracts the
// two views and composes them into one full-SBS RGBA buffer that feeds the existing SBS
// render path unchanged. That is also why MPO is not a hint fed to the layered detector
// — by the time this succeeds there is nothing left to detect.
//
// Container layout: the APP2 payload after "MPF\0" is a TIFF header (II or MM byte
// order, magic 42, u32 offset to the MP Index IFD). EVERY offset inside — the IFD
// offset, IFD value offsets, and the MP Entry image offsets — is relative to the start
// of that TIFF header, i.e. the byte just after "MPF\0", NOT to the file. The MP Index
// IFD holds tag 0xB001 (NumberOfImages) and tag 0xB002 (an array of 16-byte MP Entries:
// attribute, size, offset, and two dependency indices). Entry 0 is the primary image and
// its offset is 0, meaning "file offset 0".
//
// The input is untrusted, so every offset and length is bounds-checked against the
// buffer before use; any failure returns ok=false and the caller falls back to decoding
// the file as a plain JPEG (which works — the primary image is a normal JPEG).
//
// EYE ORDER is a convention, not a tag: MPO stores images in capture order and stereo
// cameras conventionally write the left view first, which is what we assume. Some write
// right-first; the `X` key (swap eyes) is the user's escape hatch.
#pragma once

#include "ImageDecoder.h"   // DecodedImage
#include "StereoTypes.h"    // StereoLayout

#include <string>

namespace mp {

struct MpoResult {
    DecodedImage image;                        // composed SBS (stereo) — empty if !ok
    StereoLayout layout = StereoLayout::Mono;
    bool ok = false;                           // a composed stereo image was produced
    bool stereo = false;                       // always == ok; kept for symmetry with LIF
    float autoConvergence = 0.0f;              // coarse estimate; MPO carries no baked
                                               // convergence, so this is always available
};

class MpoLoader {
public:
    // Cheap content sniff: does the JPEG header carry an APP2 "MPF\0" segment? Real MPOs
    // often ship with a .jpg extension, so dispatch must detect them by content. Reads
    // only the leading header bytes, stopping at SOS.
    static bool IsMpo(const std::string& path);

    // Parse `path` and compose its first two full-size views into a full-SBS RGBA buffer.
    // Returns ok=false for anything that is not a well-formed two-view MPO — including a
    // plain JPEG, a thumbnail-only MPF index, or mismatched view dimensions.
    static MpoResult Load(const std::string& path);
};

} // namespace mp
