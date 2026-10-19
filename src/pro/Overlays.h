// SPDX-License-Identifier: Apache-2.0
//
// pro::Overlays — framing overlays on the picture (the plan's "Overlays" P1 row): action /
// title safe, aspect frame lines with a matte, a centre cross, and burn-ins (timecode,
// frame number, clip name, colour space). Drawn by ImGui into the HUD layer over the
// picture, at the screen plane (zero parallax), so they read as a frame around the 3D
// picture. They follow pan / zoom like the depth overlay. Anamorphic desqueeze is a
// picture setting (App stretches the content), kept here with the rest of the framing.
#pragma once

#include <string>

namespace mp::pro {

struct OverlaySettings {
    bool safeAreas = false;     // action safe 93 %, title safe 90 % (EBU R 95)
    int frameLine = 0;          // index into FrameLineAspects(): 0 = off
    float matte = 0.6f;         // darkening outside the frame line, 0..1
    bool centreCross = false;
    bool burnTimecode = true;   // shown when the source has timecode
    bool burnFrame = false;
    bool burnClip = false;
    bool burnColour = false;
    int desqueeze = 0;          // index into DesqueezeFactors(): 0 = 1.0x
};

// "Off", "2.39", "1.85", "16:9", "9:16", "4:3", "1:1" and their aspect ratios.
int FrameLineCount();
const char* FrameLineName(int i);
float FrameLineAspect(int i);  // 0 for "off"
int DesqueezeCount();
const char* DesqueezeName(int i);
float DesqueezeFactor(int i);  // 1.0, 1.3, 1.5, 1.8, 2.0

// Geometry, pure (tests/overlays_test.cpp): the frame line's rectangle inside a picture of
// `pictureAspect` (w/h), as fractions of the picture (x, y, w, h), centred.
void FrameLineRect(float pictureAspect, float lineAspect, float& x, float& y, float& w, float& h);

struct BurnIn {
    std::string timecode, clip, colour;
    long long frame = -1;  // -1 = not a video
    // HUD pixels in from the picture's top / left / right edges: clears the top bar and the
    // side panels while they are up.
    float topInset = 10.0f, leftInset = 10.0f, rightInset = 10.0f;
};

// Draw into ImGui's background list. (px, py, pw, ph) is the unzoomed picture in HUD
// pixels; (panX, panY, zoom) the Pro pan / zoom, in eye UV.
void DrawOverlays(const OverlaySettings& s, const BurnIn& b, float px, float py, float pw, float ph, float panX,
                  float panY, float zoom);

}  // namespace mp::pro
