// SPDX-License-Identifier: Apache-2.0
//
// OcioBake — an OpenColorIO transform as the station's look (Media Player Pro, phase 4:
// "OpenColorIO"). The show's OCIO config is evaluated on the CPU over a 3D grid and the
// result becomes the look's 3D LUT, so it runs through the same GPU path as a .cube
// (exposure and CDL still apply before it). Built only with MP_OCIO_ROOT (the player's
// default stack has no OCIO); without it every call reports that.
//
// The input to the transform is the picture's code values as decoded (camera log over
// SDI, a log DPX, ...), in [0, 1].
#pragma once

#include "color/CubeLut.h"

#include <string>
#include <vector>

namespace mp::ocio {

bool Available();

struct Request {
    std::string config;   // path, "ocio://..." built-in, or "" = the OCIO environment variable
    std::string src;      // input colour space, e.g. "ARRI LogC4" / "ACEScct"
    std::string display;  // display + view (OCIO 2 DisplayViewTransform) ...
    std::string view;
    std::string dst;      // ... or a plain target colour space instead
    std::string looks;    // optional OCIO looks, e.g. "+ShowLook"
    int size = 65;        // grid points per axis
};

// Bake `req` into `out` (red fastest, domain 0..1). False with `err` set on any OCIO error.
bool Bake(const Request& req, color::CubeLut& out, std::string& err);

struct ConfigInfo {
    std::string name;
    std::vector<std::string> colorSpaces, displays, views;  // views of the default display
    std::string defaultDisplay, defaultView;
};
bool Describe(const std::string& config, ConfigInfo& out, std::string& err);

}  // namespace mp::ocio
