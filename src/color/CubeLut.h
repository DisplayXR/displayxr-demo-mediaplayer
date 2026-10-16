// SPDX-License-Identifier: Apache-2.0
//
// CubeLut — loader for the Adobe/Resolve `.cube` 3D LUT format, the de-facto
// interchange format for show LUTs on set (Resolve, Livegrade, Pomfort, camera
// vendors all export it). Media Player Pro applies it on the GPU as a 3D texture;
// see shaders/sbs.frag.
//
// Supported: TITLE, LUT_3D_SIZE (2..256), DOMAIN_MIN / DOMAIN_MAX, comments (#),
// blank lines, CRLF. Data order is the format's own: red varies fastest, then green,
// then blue. A 1D LUT (LUT_1D_SIZE) is rejected with a clear error rather than
// silently misread — on-set looks are 3D LUTs.
#pragma once

#include <string>
#include <vector>

namespace mp::color {

struct CubeLut {
    std::string title;
    int size = 0;                       // N: the LUT is N x N x N
    float domainMin[3] = {0.f, 0.f, 0.f};
    float domainMax[3] = {1.f, 1.f, 1.f};
    std::vector<float> rgb;             // N^3 * 3 floats, red fastest

    bool Valid() const { return size >= 2 && rgb.size() == (size_t)size * size * size * 3; }
};

// Parse `.cube` text. On failure returns false and fills `error` with a one-line
// reason that names the line number.
bool ParseCube(const std::string& text, CubeLut& out, std::string& error);

// Read + parse a `.cube` file from disk.
bool LoadCube(const std::string& path, CubeLut& out, std::string& error);

}  // namespace mp::color
