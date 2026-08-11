// SPDX-License-Identifier: Apache-2.0
//
// StereoCompose — the two operations every "container holding a stereo pair" needs:
// glue the pair into one full-SBS buffer, and estimate a convergence for a pair that
// didn't ship one.
//
// Both were extracted verbatim from LifLoader.cpp when the MPO container loader landed
// (#45); LIF and MPO now share them. Behaviour is deliberately unchanged.
//
// NOTE: EstimateAutoConvergence is SAD-based and answers "how far apart are these two
// views", which is a different question from StereoDetect's "are these two halves a
// stereo pair at all" (NCC + peak sharpness). Do not fold one onto the other — merging
// them would silently shift the convergence LIF playback has always used.
#pragma once

#include "ImageDecoder.h"   // DecodedImage

namespace mp {

// Glue two same-sized RGBA8 views into one full-SBS RGBA8 buffer, left half then right.
// Returns an invalid DecodedImage if either view is invalid or the dimensions differ.
DecodedImage ComposeSbs(const DecodedImage& left, const DecodedImage& right);

// Coarse global horizontal disparity between two same-sized views, returned as the app's
// convergence_ value that pulls the dominant plane toward the screen. Downsamples to a
// small grid, greyscales, and finds the integer shift minimizing SAD. Used only when a
// container carries no convergence field, so an under-specified stereo pair can still be
// reconverged on demand (Backspace). Returns 0 for mismatched or tiny inputs.
float EstimateAutoConvergence(const DecodedImage& L, const DecodedImage& R);

} // namespace mp
