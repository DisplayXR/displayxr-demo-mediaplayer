// SPDX-License-Identifier: Apache-2.0
//
// Target-screen depth model (#135): how the picture's depth plays on the screen it is
// made for, and how to preview that on the 3D panel.
//
// Stereo depth is set by parallax, a fraction of the picture's width. On a screen W wide
// seen from D, a parallax p (fraction of width) is a physical offset P = p W between the
// eyes' images, an angular disparity of 2 atan(P / 2D) and, past the eye separation e,
// the eyes diverge by 2 atan((P - e) / 2D). The same picture therefore looks deeper on a
// screen that fills more of the view (a cinema) and flatter on one that fills less (a
// monitor at arm's length).
//
// The preview shows the picture on the panel at the ANGULAR size it has on the target:
// scaled by k = (W_t / D_t) / (W_p / D_p) about its centre (W_p = the picture's width on
// the panel, D_p = the viewer's distance). Every parallax then subtends the angle it
// subtends on the target, framing and depth angles match (in front of / behind the
// screen, by the same angles; zero parallax stays on the screen), at the cost of cropping
// (k > 1) or a border (k < 1). Remapping depth without changing the picture's size needs
// view synthesis, which is a later step.
#pragma once

#include <string>
#include <vector>

namespace mp::pro::target {

constexpr float kEyeSeparationM = 0.065f;  // adult interocular, the usual reference
constexpr float kDivergenceCapDeg = 0.5f;  // background divergence alert (plan review)

struct Screen {
    std::string name;       // "Cinema", "Home TV", ... or "Custom"
    float widthM = 0.0f;    // the picture's width on that screen
    float distanceM = 0.0f; // the viewer's distance from it
    bool On() const { return widthM > 0.0f && distanceM > 0.0f; }
};

// Cinema 10 m at 12 m, Home TV 1.44 m (65") at 2.5 m, Monitor 0.60 m (27") at 0.65 m,
// Tablet 0.25 m (11") at 0.40 m.
const std::vector<Screen>& Presets();
// A preset by name (case-insensitive, "tv" / "home_tv" / "home tv" all match); false if none.
bool PresetByName(const std::string& name, Screen& out);

// The preview scale for the panel: (W_t / D_t) / (W_p / D_p). 1 when either side is unknown.
float PictureScale(const Screen& t, float panelPictureWidthM, float panelViewerDistanceM);

// One parallax (% of the picture's width, + = behind the screen) on the target.
struct Depth {
    float mm = 0.0f;             // physical parallax on the target screen
    float arcmin = 0.0f;         // angular disparity at the viewing distance (signed)
    float divergenceDeg = 0.0f;  // how far the eyes diverge (0 unless behind past e)
};
Depth AtParallax(const Screen& t, float pct, float eyeSepM = kEyeSeparationM);

// The far parallax (% of width) at which the target's background diverges by `capDeg`.
float DivergenceLimitPct(const Screen& t, float capDeg = kDivergenceCapDeg, float eyeSepM = kEyeSeparationM);

}  // namespace mp::pro::target
