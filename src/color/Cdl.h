// SPDX-License-Identifier: Apache-2.0
//
// ASC CDL (Color Decision List) — the slope/offset/power + saturation grade that
// travels from set to post. Media Player Pro reads the three common carriers:
//   .cc   one <ColorCorrection>
//   .ccc  a <ColorCorrectionCollection>; the first correction (or the one whose id
//         matches `wantId`) is used
//   .cdl  a <ColorDecisionList>; same rule as .ccc
// The parser is deliberately tolerant: it looks for the <Slope>, <Offset>, <Power>
// and <Saturation> elements inside the chosen correction and ignores everything else
// (namespaces, <Description>, <InputDescription>...).
//
// Applied on the GPU per ASC v1.2: out = clamp(in * slope + offset)^power, then
// saturation around Rec.709 luma. See shaders/sbs.frag.
#pragma once

#include <string>

namespace mp::color {

struct Cdl {
    std::string id;
    float slope[3] = {1.f, 1.f, 1.f};
    float offset[3] = {0.f, 0.f, 0.f};
    float power[3] = {1.f, 1.f, 1.f};
    float saturation = 1.f;

    bool IsIdentity() const {
        for (int i = 0; i < 3; ++i)
            if (slope[i] != 1.f || offset[i] != 0.f || power[i] != 1.f) return false;
        return saturation == 1.f;
    }
};

bool ParseCdl(const std::string& xml, Cdl& out, std::string& error, const std::string& wantId = {});
bool LoadCdl(const std::string& path, Cdl& out, std::string& error, const std::string& wantId = {});

}  // namespace mp::color
