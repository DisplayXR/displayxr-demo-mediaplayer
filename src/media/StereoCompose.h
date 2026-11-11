// SPDX-License-Identifier: Apache-2.0
//
// StereoCompose — the two operations every "container holding a stereo pair" needs:
// glue the pair into one full-SBS buffer, and estimate a convergence for a pair that
// didn't ship one.
//
// Both were extracted verbatim from LifLoader.cpp when the MPO container loader landed
// (#45); LIF and MPO now share them. Behaviour is deliberately unchanged.
//
// NOTE: EstimateAutoConvergence answers "how far is the subject from the screen plane",
// which is a different question from StereoDetect's "are these two halves a stereo pair
// at all" (NCC + peak sharpness). Do not fold one onto the other.
#pragma once

#include "ImageDecoder.h"   // DecodedImage

namespace mp {

// Glue two same-sized RGBA8 views into one full-SBS RGBA8 buffer, left half then right.
// Returns an invalid DecodedImage if either view is invalid or the dimensions differ.
DecodedImage ComposeSbs(const DecodedImage& left, const DecodedImage& right);

// The per-eye shift (fraction of the eye's width, +left / -right) for a composed full-SBS
// pair whose container carries no convergence, applied by default (#180). It is the
// auto-convergence measurement (autoconv::MeasureStillShift: the nearest subject at the
// screen plane). When that finds no confident match, a coarse global SAD shift of the two
// halves stands in, which pulls the dominant plane to the screen. 0 for tiny inputs.
float EstimateAutoConvergence(const DecodedImage& sbs);

} // namespace mp
