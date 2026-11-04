// SPDX-License-Identifier: Apache-2.0
//
// SpatialMedia (#154) — the pure, libav-free half of reading phone "spatial" capture:
// MV-HEVC stereo video (one HEVC stream, two coded views) and stereo HEIC photos (two images
// tied together by a HEIF `ster` entity group). Both are what Apple's spatial capture and the
// Samsung Galaxy "3D Capture" mode write.
//
// Both formats carry two FULL views, and both are composed into ONE full side-by-side
// picture before anything else sees them (VideoDecoder for MV-HEVC, HeifLoader for HEIC).
// That is the same move MpoLoader and LifLoader make: the rest of the player — the
// renderer, the per-eye UV split, auto-convergence (#153), capture, the layout HUD — keeps
// one picture per frame and needs no second-eye path. The cost is one CPU copy of each
// decoded view into the composite, which software MV-HEVC decode dwarfs anyway.
//
// What lives here is the part worth unit-testing without a decoder:
//   * which of the two views is the LEFT eye (metadata first, base view = left otherwise);
//   * a bounds-checked walk of a HEIF file's `meta` box for its `ster` group and `pitm`.
//
// This header and SpatialMedia.cpp depend on the C++ standard library only, so the tests
// link them without FFmpeg and Android could take them unchanged later.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mp::spatial {

// What one decoded view says about itself. MV-HEVC streams name each view's eye in the
// 3D reference displays SEI / the VPS extension and FFmpeg forwards that as AVStereo3D.view
// on the frame; a stream that doesn't say leaves it Unknown.
enum class EyeHint { Unknown, Left, Right };

// The verdict for one pair of views, in decode order (A came out of the decoder first).
struct EyeOrder {
    bool firstIsLeft = true;   // A is the left eye
    bool fromMetadata = false; // decided by the stream's own eye tags, not the fallback
};

// Decide which of two views is the left eye.
//   1. An explicit tag wins: A says Left or B says Right -> A is left; A says Right or B says
//      Left -> B is left. Contradictory tags (both Left, both Right) fall through.
//   2. Otherwise the view with the LOWER view id is left: the base view of a stereo MV-HEVC
//      stream is the left eye unless the stream says otherwise, which is also how the format
//      degrades to 2D (a decoder that only knows the base layer shows the left eye).
// viewIdA/B are the views' ids, or -1 when unknown (then A — the base view — is left).
// Inline so VideoDecoder.cpp stays linkable on its own (several tests link just that TU).
inline EyeOrder ResolveEyeOrder(EyeHint a, EyeHint b, int viewIdA, int viewIdB) {
    EyeOrder o;
    const bool aLeft = (a == EyeHint::Left) || (b == EyeHint::Right);
    const bool bLeft = (a == EyeHint::Right) || (b == EyeHint::Left);
    if (aLeft != bLeft) {  // exactly one reading: the tags agree (or only one spoke)
        o.firstIsLeft = aLeft;
        o.fromMetadata = true;
        return o;
    }
    // No tags, or tags that contradict each other: the base (lower id) view is left.
    if (viewIdA >= 0 && viewIdB >= 0 && viewIdA != viewIdB) o.firstIsLeft = viewIdA < viewIdB;
    else o.firstIsLeft = true;  // decode order: the base view comes out first
    return o;
}

// The stereo pair a HEIF file declares, plus its primary item (the one every ordinary
// viewer shows). Item ids are the file's own (`iinf`), which is also what FFmpeg's mov
// demuxer exposes as AVStream::id (a coded image item) or AVStreamGroup::id (a grid).
struct HeifStereoPair {
    bool hasPair = false;      // a `ster` group with at least two entities was found
    uint32_t leftId = 0;       // ISO/IEC 23008-12 `ster`: entity 0 is the LEFT view,
    uint32_t rightId = 0;      //                          entity 1 the right one
    bool hasPrimary = false;
    uint32_t primaryId = 0;    // `pitm`
};

// Walk the top-level boxes of a HEIF file for `meta`, then inside it for `pitm` and
// `grpl/ster`. Every size and offset is checked against `size`; anything malformed simply
// yields hasPair=false (the caller then shows the primary image, flat). `data` should be
// the whole file, or at least a prefix that contains the `meta` box.
HeifStereoPair ParseHeifStereoPair(const uint8_t* data, size_t size);

// Does this look like a HEIF still (an `ftyp` whose major or compatible brands include
// heic/heix/heim/heis/mif1/msf1 and friends)? Reads the first box only. HEIF movies that
// are really MP4 (brand `msf1` alone) still say yes; the loader copes with either.
bool LooksLikeHeif(const uint8_t* data, size_t size);

}  // namespace mp::spatial
