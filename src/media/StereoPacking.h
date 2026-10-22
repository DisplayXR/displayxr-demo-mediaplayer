// SPDX-License-Identifier: Apache-2.0
//
// StereoPacking (#128) — how a packed stereo frame is sliced into two eyes.
//
// One place for the geometry every packed layout shares: which normalised rect of the
// decoded frame each eye samples, and the aspect one eye is shown at. Side-by-side packs
// the eyes across the width (left eye on the left), top-bottom ("over-under") packs them
// down the height (left eye on top, the common convention). A FULL packing keeps each
// eye at its native resolution, so an eye is half the frame; a HALF packing squeezed
// each eye into half the frame, so it is stretched back to the full frame's aspect.

#pragma once

#include "media/StereoTypes.h"

namespace mp::packing {

struct EyeRect {
    float x, y, w, h;  // normalised to the decoded frame
};

inline bool IsSbs(StereoLayout l) { return l == StereoLayout::SbsFull || l == StereoLayout::SbsHalf; }
inline bool IsTb(StereoLayout l) { return l == StereoLayout::TbFull || l == StereoLayout::TbHalf; }
inline bool IsPacked(StereoLayout l) { return IsSbs(l) || IsTb(l); }

// The rect one eye samples. Unpacked layouts (mono, dual) sample the whole frame.
inline EyeRect Eye(StereoLayout l, bool left) {
    if (IsSbs(l)) return left ? EyeRect{0.0f, 0.0f, 0.5f, 1.0f} : EyeRect{0.5f, 0.0f, 0.5f, 1.0f};
    if (IsTb(l)) return left ? EyeRect{0.0f, 0.0f, 1.0f, 0.5f} : EyeRect{0.0f, 0.5f, 1.0f, 0.5f};
    return EyeRect{0.0f, 0.0f, 1.0f, 1.0f};
}

// Display aspect (width / height) of one eye.
inline float PerEyeAspect(StereoLayout l, int frameW, int frameH) {
    if (frameW <= 0 || frameH <= 0) return 1.0f;
    const float w = (float)frameW, h = (float)frameH;
    if (l == StereoLayout::SbsFull) return 0.5f * w / h;
    if (l == StereoLayout::TbFull) return w / (0.5f * h);
    return w / h;  // mono, dual, and the HALF packings (stretched back to the frame)
}

// Top-bottom metadata and names say nothing about full vs half; the frame aspect does.
// A full top-bottom frame stacks two full eyes, so it is tall (16:9 eyes -> 0.89); a
// half one keeps the eye aspect (1.78). Nothing sane sits between, so split at 1.2.
inline StereoLayout ChooseTbFullOrHalf(float frameAspect) {
    return (frameAspect > 0.0f && frameAspect < 1.2f) ? StereoLayout::TbFull : StereoLayout::TbHalf;
}

}  // namespace mp::packing
