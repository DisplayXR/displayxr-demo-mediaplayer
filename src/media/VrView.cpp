// SPDX-License-Identifier: Apache-2.0
#include "media/VrView.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace mp::vr {

const char* ProjectionName(Projection p) {
    switch (p) {
        case Projection::Equirect180: return "VR180";
        case Projection::Equirect360: return "VR360";
        case Projection::Fisheye: return "VR180 fisheye";
        case Projection::Cubemap: return "360 cubemap";
        case Projection::Eac: return "360 EAC";
        default: return "flat";
    }
}

const char* ProjectionId(Projection p) {
    switch (p) {
        case Projection::Equirect180: return "equirect180";
        case Projection::Equirect360: return "equirect360";
        case Projection::Fisheye: return "fisheye";
        case Projection::Cubemap: return "cubemap";
        case Projection::Eac: return "eac";
        default: return "flat";
    }
}

bool ParseProjectionId(const std::string& s, Projection& out) {
    if (s == "flat" || s == "none") out = Projection::None;
    else if (s == "equirect180" || s == "vr180") out = Projection::Equirect180;
    else if (s == "equirect360" || s == "vr360") out = Projection::Equirect360;
    else if (s == "fisheye" || s == "fisheye180") out = Projection::Fisheye;
    else if (s == "cubemap" || s == "c3x2") out = Projection::Cubemap;
    else if (s == "eac") out = Projection::Eac;
    else return false;
    return true;
}

namespace {
float WrapDeg(float d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}
float ClampF(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
// Each eye's own aspect: 2:1 for a 360 equirect, 3:2 for a 3x2 cubemap / EAC, 1:1 for a
// half sphere / fisheye circle.
float EyeAspect(Projection p) {
    if (p == Projection::Equirect360) return 2.0f;
    if (IsCube(p)) return 1.5f;
    return 1.0f;
}
}  // namespace

void Clamp(ViewState& v, Projection p, bool stereo, float hfovDeg, float aspect, const Limits& lim) {
    if (v.fovDeg != 0.0f) v.fovDeg = ClampF(v.fovDeg, lim.fovMinDeg, lim.fovMaxDeg);
    if (p == Projection::None) {
        v = ViewState{};
        return;
    }
    const float hw = std::tan(0.5f * Rad(ClampF(hfovDeg, lim.fovMinDeg, lim.fovMaxDeg)));
    const float hh = hw / (aspect > 0.f ? aspect : 1.f);
    const float halfV = Deg(std::atan(hh));
    const Model m = ModelFor(p, stereo);
    const bool wraps = IsFullSphere(p);

    // Pitch: never past the configured limit, and the window's top / bottom edge stays
    // short of the pole (where the rotation would fold the picture over).
    const float pitchMax = std::max(0.0f, std::min(lim.pitchMaxDeg, 89.0f - halfV));
    v.pitchDeg = ClampF(v.pitchDeg, -pitchMax, pitchMax);

    if (m == Model::Plane) {
        // The window centre's off-axis angle, and its outer edge short of +-88 degrees
        // (the plane runs to infinity at 90).
        const float edgeMax = std::tan(Rad(88.0f)) - hw;
        const float panMax = std::max(0.0f, std::min(lim.panMaxDeg, edgeMax > 0.f ? Deg(std::atan(edgeMax)) : 0.f));
        v.yawDeg = ClampF(v.yawDeg, -panMax, panMax);
    } else if (wraps) {
        v.yawDeg = WrapDeg(v.yawDeg);
    } else {
        // Mono 180: the heading keeps the window inside the half sphere.
        const float halfH = Deg(std::atan(hw));
        const float yawMax = std::max(0.0f, 90.0f - halfH);
        v.yawDeg = ClampF(v.yawDeg, -yawMax, yawMax);
    }
}

EyeWindow ComputeWindow(Projection p, bool stereo, const ViewState& v, float hfovDeg, float aspect,
                        float shiftFrac) {
    EyeWindow w;
    w.model = ModelFor(p, stereo);
    const float hw = std::tan(0.5f * Rad(hfovDeg));
    const float hh = hw / (aspect > 0.f ? aspect : 1.f);
    w.sx = 2.0f * hw;
    w.sy = -2.0f * hh;
    w.x0 = -hw;
    w.y0 = hh;
    switch (w.model) {
        case Model::Plane:  // off-axis window across the baseline-parallel plane; pitch rotates
            w.x0 = std::tan(Rad(v.yawDeg)) - hw;
            w.pitchRad = Rad(v.pitchDeg);
            break;
        case Model::Cylinder:  // heading rotates; pitch is a vertical shift in tan(elevation)
            w.headingRad = Rad(v.yawDeg);
            w.y0 = std::tan(Rad(v.pitchDeg)) + hh;
            break;
        case Model::Free:
            w.headingRad = Rad(v.yawDeg);
            w.pitchRad = Rad(v.pitchDeg);
            break;
    }
    w.x0 -= shiftFrac * w.sx;  // the picture moves right = the window moves left
    return w;
}

Projection FromSpherical(int kind, float horizCoverageDeg) {
    switch (kind) {
        case 0:
        case 1:
            if (horizCoverageDeg <= 0.0f) return Projection::None;
            return horizCoverageDeg >= 270.0f ? Projection::Equirect360 : Projection::Equirect180;
        case 2: return Projection::Equirect180;
        case 3: return Projection::Fisheye;
        case 4: return Projection::Cubemap;
        default: return Projection::None;
    }
}

bool ProjectionFromFilename(const std::string& path, Projection& out, int& packingHint) {
    std::string base = path;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    bool is180 = false, is360 = false, fish = false, equi = false, cube = false, eac = false;
    packingHint = -1;
    std::string tok;
    for (size_t i = 0; i <= base.size(); ++i) {
        const char ch = i < base.size() ? base[i] : '_';
        if (std::isalnum((unsigned char)ch)) {
            tok += ch;
            continue;
        }
        if (!tok.empty()) {
            if (tok == "vr180" || tok == "180" || tok == "180x180" || tok == "180x180_3dh") is180 = true;
            else if (tok == "vr360" || tok == "360" || tok == "360x180") is360 = true;
            else if (tok == "fisheye" || tok == "fisheye180" || tok == "dualfisheye") fish = true;
            else if (tok == "equirect" || tok == "erp" || tok == "equirectangular") equi = true;
            else if (tok == "cubemap" || tok == "c3x2" || tok == "cbmp") cube = true;
            else if (tok == "eac") eac = true;
            else if (tok == "3dh" || tok == "sbs" || tok == "lr" || tok == "rl") packingHint = 1;
            else if (tok == "3dv" || tok == "tb" || tok == "ou" || tok == "bt") packingHint = 2;
            else if (tok == "mono" || tok == "2d") packingHint = 0;
        }
        tok.clear();
    }
    if (fish) out = Projection::Fisheye;
    else if (eac) out = Projection::Eac;
    else if (cube) out = Projection::Cubemap;
    else if (is360) out = Projection::Equirect360;
    else if (is180) out = Projection::Equirect180;
    else if (equi) out = Projection::Equirect360;  // a bare "equirect" is the full sphere
    else return false;
    return true;
}

StereoLayout PackingFromAspect(Projection p, float a) {
    if (p == Projection::None || !(a > 0.0f)) return StereoLayout::Mono;
    const float eye = EyeAspect(p);
    // Pick the packing whose frame aspect is nearest in log terms: SBS 2x, mono 1x, TB 0.5x.
    const float r = std::log(a / eye);
    if (r > std::log(1.41421356f)) return StereoLayout::SbsFull;
    if (r < -std::log(1.41421356f)) return StereoLayout::TbFull;
    return StereoLayout::Mono;
}

bool AspectPlausible(Projection p, float a) {
    if (p == Projection::None || !(a > 0.0f)) return false;
    const float eye = EyeAspect(p);
    for (float k : {0.5f, 1.0f, 2.0f})
        if (std::fabs(std::log(a / (eye * k))) < std::log(1.15f)) return true;
    // YouTube EAC: 16:9 frames, mono or side by side (each eye then a portrait 8:9).
    if (p == Projection::Eac && std::fabs(std::log(a / (16.0f / 9.0f))) < std::log(1.05f)) return true;
    return false;
}

void RenderEyeGray(const uint8_t* src, int srcW, int srcH, int srcStride, packing::EyeRect rect, Projection p,
                   float fisheyeFovDeg, const EyeWindow& w, uint8_t* dst, int dstW, int dstH, int dstStride,
                   int cubePadPx) {
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    const float eyePxW = rect.w * (float)srcW, eyePxH = rect.h * (float)srcH;
    const float eyeAspect = eyePxH > 0.f ? eyePxW / eyePxH : 1.f;
    for (int y = 0; y < dstH; ++y) {
        uint8_t* row = dst + (std::ptrdiff_t)y * dstStride;
        const float t = ((float)y + 0.5f) / (float)dstH;
        for (int x = 0; x < dstW; ++x) {
            const float s = ((float)x + 0.5f) / (float)dstW;
            float u = 0.f, v = 0.f;
            if (!SampleUV(w, p, fisheyeFovDeg, eyeAspect, s, t, u, v, eyePxW, eyePxH, (float)cubePadPx)) {
                row[x] = 0;
                continue;
            }
            // Bilinear, texel centres at +0.5, clamped inside the eye rect (as the shader).
            const float fx = (rect.x + u * rect.w) * (float)srcW - 0.5f;
            const float fy = (rect.y + v * rect.h) * (float)srcH - 0.5f;
            const int ex0 = (int)(rect.x * (float)srcW), ex1 = (int)((rect.x + rect.w) * (float)srcW + 0.5f) - 1;
            const int ey0 = (int)(rect.y * (float)srcH), ey1 = (int)((rect.y + rect.h) * (float)srcH + 0.5f) - 1;
            const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
            const float ax = fx - (float)ix, ay = fy - (float)iy;
            auto at = [&](int xx, int yy) -> float {
                xx = xx < ex0 ? ex0 : xx > ex1 ? ex1 : xx;
                yy = yy < ey0 ? ey0 : yy > ey1 ? ey1 : yy;
                return (float)src[(std::ptrdiff_t)yy * srcStride + xx];
            };
            const float top = at(ix, iy) * (1.f - ax) + at(ix + 1, iy) * ax;
            const float bot = at(ix, iy + 1) * (1.f - ax) + at(ix + 1, iy + 1) * ax;
            const float val = top * (1.f - ay) + bot * ay;
            row[x] = (uint8_t)(val + 0.5f);
        }
    }
}

}  // namespace mp::vr
