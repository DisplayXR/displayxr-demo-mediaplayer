// SPDX-License-Identifier: Apache-2.0
//
// VrView (#155) — the VR180 / VR360 viewer: a mouse-steered window into a spherical
// stereo source, with the stereo pair kept RECTIFIED (rows aligned) at every view.
//
// A VR source is not a packing, it is a projection. Each eye of a packed frame (SBS / TB /
// whole) holds that eye's picture of the sphere: equirectangular over a half (VR180) or a
// full (VR360) sphere, or a raw fisheye. The viewer samples each eye through a virtual
// camera window and feeds the result to the ordinary two-eye path. This header is the
// whole of that geometry, Vulkan-free so it is unit-tested; shaders/sbs.frag (VrLocalUV)
// mirrors SampleUV() line for line.
//
// WHY THE WINDOW IS NOT A ROTATING CAMERA (the point of the design):
//
//   The two source cameras sit on a fixed horizontal baseline (x). Two pinhole images of
//   them are a rectified pair - every scene point lands on the same row in both - exactly
//   when both image planes share an orientation that CONTAINS the baseline direction.
//
//   * Pitch rotates about the baseline axis, which keeps x in the image plane: a real
//     rotation, rows stay aligned.
//   * Yaw would turn the baseline out of the image plane. Off-centre that adds vertical
//     disparity and foreshortens the baseline by ~cos(yaw) (depth flattens), which is
//     uncomfortable by ~45 degrees. So yaw does NOT rotate anything: both eyes keep their
//     image plane parallel to the baseline and the view pans with an OFF-AXIS window -
//     a lens shift across that plane, the same off-axis idea as the runtime's Kooima
//     projection for the panel. Rows stay aligned at every pan; the price is stretching
//     toward +-90 degrees, hence the pan clamp (Limits::panMaxDeg, default 65).
//   * Roll is locked to the horizon.
//
//   That is Model::Plane, used for stereo VR180 equirect and stereo fisheye.
//
//   Stereo VR360 is omni-directional stereo (ODS): each COLUMN of a 360 pair was captured
//   by a camera pair whose baseline is perpendicular to that column's azimuth, so there is
//   no single baseline and no pinhole window is exactly rectified off-centre. What ODS
//   does guarantee is that a scene point has the SAME elevation in both eyes. Model::
//   Cylinder makes the output row a function of elevation alone (row = tan(elevation),
//   column = azimuth through atan) - a cylindrical window - so rows align exactly at every
//   heading, which wraps freely; pitch is the same kind of vertical lens shift.
//
//   A mono source has no pair to keep rectified: Model::Free is a plain rotating camera
//   (yaw and pitch both real rotations).
//
// COORDINATES: camera space is x right, y up, z forward (the source camera's forward at
// the centre of the eye image). A window maps the eye viewport's (s, t) in [0,1]^2 (t
// DOWN, like the shader's vUV) to tangent-plane coordinates (tx, ty) = (x0 + s*sx,
// y0 + t*sy). Longitude is 0 at the eye image's centre column and positive to the right;
// latitude is positive up. Eye-local source uv is [0,1]^2 over ONE eye's rect, v down.
#pragma once

#include "media/StereoPacking.h"
#include "media/StereoTypes.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace mp::vr {

constexpr float kPi = 3.14159265358979323846f;
inline float Rad(float d) { return d * (kPi / 180.0f); }
inline float Deg(float r) { return r * (180.0f / kPi); }

enum class Projection : uint8_t {
    None,         // a flat picture: the ordinary player
    Equirect180,  // equirectangular, 180 x 180 degrees per eye (YouTube VR180)
    Equirect360,  // equirectangular, 360 x 180 degrees per eye
    Fisheye,      // equidistant fisheye per eye (raw VR180 cameras), FOV Format::fisheyeFovDeg
};

enum class Model : uint8_t {
    Plane = 0,     // stereo, fixed baseline: pitch rotates, yaw is an off-axis window
    Cylinder = 1,  // stereo ODS 360: row = tan(elevation), heading wraps
    Free = 2,      // mono: yaw and pitch both rotate
};

const char* ProjectionName(Projection p);  // HUD: "flat", "VR180", "VR360", "VR180 fisheye"
const char* ProjectionId(Projection p);    // MCP: "flat", "equirect180", "equirect360", "fisheye"
bool ParseProjectionId(const std::string& s, Projection& out);  // also accepts "none"

inline bool IsStereoLayout(StereoLayout l) { return packing::IsPacked(l); }

inline Model ModelFor(Projection p, bool stereo) {
    if (!stereo) return Model::Free;
    return p == Projection::Equirect360 ? Model::Cylinder : Model::Plane;
}

// Pan / pitch / zoom bounds. Configurable (MEDIAPLAYER_VR_PAN_MAX, ...): the pan clamp is
// where the off-axis window's stretch (1/cos^2 across, 1/cos down) becomes the trade.
struct Limits {
    float panMaxDeg = 65.0f;    // Plane model: |yaw| of the window centre
    float pitchMaxDeg = 60.0f;  // all models
    float fovMinDeg = 10.0f;    // horizontal FOV zoom range
    float fovMaxDeg = 120.0f;
};

// What the user steers. yaw: Plane = the off-axis window centre's angle; Cylinder / Free =
// the heading (wraps on 360). fovDeg = horizontal field of view of the window, 0 = the
// life-size default (LifeSizeHFovDeg).
struct ViewState {
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
    float fovDeg = 0.0f;
};

// The panel's real angular width at the viewing distance: the window shown that wide
// looks life-size. `windowWidthM` = the window's physical width on the panel.
inline float LifeSizeHFovDeg(float windowWidthM, float viewerDistM) {
    if (!(windowWidthM > 0.0f) || !(viewerDistM > 0.0f)) return 40.0f;
    return Deg(2.0f * std::atan(0.5f * windowWidthM / viewerDistM));
}

// Clamp a view to its limits. `hfovDeg` = the effective horizontal FOV (resolved default),
// `aspect` = window width / height. The window edges never reach the sphere's +-90 (180
// sources) and pitch keeps the top / bottom edge short of the pole.
void Clamp(ViewState& v, Projection p, bool stereo, float hfovDeg, float aspect, const Limits& lim);

// One eye's sampling window (what the shader receives).
struct EyeWindow {
    float x0 = 0.f, y0 = 0.f, sx = 1.f, sy = -1.f;  // (tx, ty) = (x0 + s*sx, y0 + t*sy)
    float pitchRad = 0.f;                           // Plane / Free: rotation about x
    float headingRad = 0.f;                         // Cylinder / Free: rotation about y
    Model model = Model::Plane;
};

// The window for one eye. `shiftFrac` = how far the picture moves RIGHT, as a fraction of
// the eye's width (convergence: +shift for the left eye, -shift for the right, the same
// numbers the flat path turns into a pixel offset). In the VR viewer it is a horizontal
// lens shift of the window - still rectified - so it never exposes a de-occlusion strip.
EyeWindow ComputeWindow(Projection p, bool stereo, const ViewState& v, float hfovDeg, float aspect,
                        float shiftFrac);

// World direction of the ray through tangent coordinates (tx, ty).
inline void RayDir(const EyeWindow& w, float tx, float ty, float d[3]) {
    if (w.model == Model::Cylinder) {
        const float a = w.headingRad + std::atan(tx);
        const float e = std::atan(ty);
        d[0] = std::cos(e) * std::sin(a);
        d[1] = std::sin(e);
        d[2] = std::cos(e) * std::cos(a);
        return;
    }
    const float cp = std::cos(w.pitchRad), sp = std::sin(w.pitchRad);
    const float cx = tx, cy = ty * cp + sp, cz = -ty * sp + cp;
    const float ch = std::cos(w.headingRad), sh = std::sin(w.headingRad);
    d[0] = cx * ch + cz * sh;
    d[1] = cy;
    d[2] = -cx * sh + cz * ch;
}

// Eye-local source uv of a world direction. `eyeAspect` = the eye rect's pixel width /
// height (the fisheye image circle is inscribed in its shorter side). Returns false where
// the projection has no picture (behind a 180 source, outside the fisheye circle).
inline bool DirToLocalUV(Projection p, float fisheyeFovDeg, float eyeAspect, const float d[3], float& u,
                         float& v) {
    if (p == Projection::Fisheye) {
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float c = len > 0.f ? d[2] / len : 1.f;
        c = c < -1.f ? -1.f : c > 1.f ? 1.f : c;
        const float th = std::acos(c);
        const float half = 0.5f * Rad(fisheyeFovDeg);
        const float r = 0.5f * th / half;
        const float rho = std::sqrt(d[0] * d[0] + d[1] * d[1]);
        const float dx = rho > 1e-8f ? d[0] / rho : 0.f, dy = rho > 1e-8f ? d[1] / rho : 0.f;
        const float kx = eyeAspect > 1.f ? 1.f / eyeAspect : 1.f;
        const float ky = eyeAspect > 1.f ? 1.f : eyeAspect;
        u = 0.5f + dx * r * kx;
        v = 0.5f - dy * r * ky;
        return th <= half;
    }
    const float lon = std::atan2(d[0], d[2]);
    const float lat = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
    v = 0.5f - lat / kPi;
    if (p == Projection::Equirect180) {
        u = lon / kPi + 0.5f;
        return std::fabs(lon) <= 0.5f * kPi;
    }
    const float x = lon / (2.0f * kPi) + 0.5f;  // Equirect360: wraps
    u = x - std::floor(x);
    return true;
}

// The whole per-pixel mapping: eye viewport (s, t) -> eye-local source uv. This is what
// shaders/sbs.frag VrLocalUV computes; keep them identical.
inline bool SampleUV(const EyeWindow& w, Projection p, float fisheyeFovDeg, float eyeAspect, float s, float t,
                     float& u, float& v) {
    float d[3];
    RayDir(w, w.x0 + s * w.sx, w.y0 + t * w.sy, d);
    return DirToLocalUV(p, fisheyeFovDeg, eyeAspect, d, u, v);
}

// The eye rect a VR source samples: SBS / TB halves, or the whole frame. Full vs half does
// not matter here - the projection, not the frame aspect, says how wide an eye is.
inline packing::EyeRect EyeRectFor(StereoLayout l, bool secondEye) {
    if (packing::IsSbs(l)) return secondEye ? packing::EyeRect{0.5f, 0.f, 0.5f, 1.f} : packing::EyeRect{0.f, 0.f, 0.5f, 1.f};
    if (packing::IsTb(l)) return secondEye ? packing::EyeRect{0.f, 0.5f, 1.f, 0.5f} : packing::EyeRect{0.f, 0.f, 1.f, 0.5f};
    return packing::EyeRect{0.f, 0.f, 1.f, 1.f};
}

// ---- the shader's push block --------------------------------------------------------
//
// The desktop push block is at Vulkan's guaranteed 128 bytes, so the VR state rides spare
// bits (rhi/VulkanRenderer.cpp, shaders/sbs.frag): the tangent window is uvOffset/uvScale,
// the flags below sit in `mode` above the 2-bit source mode, the heading in viewFlags'
// high 16 bits and the pitch in cdlPower.a (the CDL power is RGB only).
//
//   mode bits 8-9 projection, 10-11 model, 12-13 packing (0 whole, 1 SBS, 2 TB),
//   14 = the second eye's rect, 16-24 fisheye FOV in degrees.
inline uint32_t PackModeFlags(Projection p, Model m, StereoLayout l, bool secondEye, float fisheyeFovDeg) {
    if (p == Projection::None) return 0;
    const uint32_t pk = packing::IsSbs(l) ? 1u : packing::IsTb(l) ? 2u : 0u;
    int fov = (int)(fisheyeFovDeg + 0.5f);
    fov = fov < 1 ? 1 : fov > 511 ? 511 : fov;
    return ((uint32_t)p << 8) | ((uint32_t)m << 10) | (pk << 12) | ((secondEye ? 1u : 0u) << 14) |
           ((uint32_t)fov << 16);
}
// Heading as unorm16 of a turn, for viewFlags' high half.
inline uint32_t PackHeading(float headingRad) {
    float turns = headingRad / (2.0f * kPi);
    turns -= std::floor(turns);
    uint32_t q = (uint32_t)(turns * 65536.0f + 0.5f);
    return (q & 0xFFFFu) << 16;
}
inline float UnpackHeading(uint32_t viewFlags) { return (float)(viewFlags >> 16) / 65536.0f * 2.0f * kPi; }

// ---- detection ----------------------------------------------------------------------

// The container's spherical metadata, as FFmpeg reports it (AVSphericalMapping), reduced
// to a projection. `kind`: 0 equirect, 1 equirect tile (bounded: VR180 writes its 180
// degrees as a 360 equirect cropped to the middle half), 2 half equirect (Apple 'hequ'),
// 3 fisheye, anything else unsupported. `horizCoverageDeg` = the bounded width for 0 / 1.
Projection FromSpherical(int kind, float horizCoverageDeg);

// File-name hints: tokens of the FILE name (not the folder) such as `vr180`, `180`,
// `180x180`, `360`, `vr360`, `fisheye`, `equirect`, plus the packing tokens VR releases
// use (`3dh` / `sbs` / `lr` = side by side, `3dv` / `tb` / `ou` = top-bottom, `mono`).
// `packingHint` = -1 none, 0 mono, 1 SBS, 2 TB. False when the name names no projection.
bool ProjectionFromFilename(const std::string& path, Projection& out, int& packingHint);

// The packing a VR frame's aspect implies (each eye's picture has a known aspect: 1:1 for
// VR180 / fisheye, 2:1 for VR360): 360 mono 2:1, 360 TB 1:1, 360 SBS 4:1; 180 SBS 2:1,
// 180 mono 1:1, 180 TB 1:2.
StereoLayout PackingFromAspect(Projection p, float frameAspect);
// True when the frame aspect is within ~15 % of one of those packings. A bare `180` / `360`
// file-name token is honoured only then, so `frame_180.png` stays a flat picture.
bool AspectPlausible(Projection p, float frameAspect);

// ---- CPU sampling (auto-convergence on the rendered window, tests) -------------------

// Render one eye's window from an 8-bit luma frame (bilinear, clamped to the eye rect;
// outside the projection = 0).
void RenderEyeGray(const uint8_t* src, int srcW, int srcH, int srcStride, packing::EyeRect rect, Projection p,
                   float fisheyeFovDeg, const EyeWindow& w, uint8_t* dst, int dstW, int dstH, int dstStride);

}  // namespace mp::vr
