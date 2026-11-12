// SPDX-License-Identifier: Apache-2.0
//
// TapHandler: whether the touch player's chrome (top bar, transport, 2D/3D switch) shows
// (#180). It shows for 3 s after a touch, and while held (a paused video). A tap on the
// picture shows it, or hides it if it showed when the finger went down; a tap on the
// chrome is the chrome's. Once a tap hides it, it stays hidden until the next tap. Pure
// logic, render thread. Times in seconds, positions in pixels.
#pragma once

namespace mp::ui {

class TapHandler {
public:
    // How far a press may move and still be a tap.
    void SetSlop(float slop) { slop_ = slop; }
    // A held chrome shows whatever the time.
    void SetHeld(bool held) { held_ = held; }

    // A finger down. `onChrome`: over a control as last drawn, only read while the chrome
    // shows. Returns whether it showed: the press is then the chrome's to handle too.
    bool Press(float x, float y, double t, bool onChrome);
    void Move(float x, float y, double t);
    void Release(float x, float y, double t);
    // The press ends without a tap (the system took it, a second finger).
    void Cancel(double t);

    // Show the chrome for a while from `t`, unless a tap hid it (a file opened).
    void Wake(double t) { lastTouch_ = t; }
    // Show it for a while from `t`, also after a tap hid it (a touch outside the player).
    void Show(double t);

    bool Shown(double t) const;

private:
    static constexpr double kShowSeconds = 3.0;

    float slop_ = 12.0f;
    bool held_ = false;
    bool hidden_ = false;  // a tap hid it
    double lastTouch_ = -1e9;

    // The press under way: whether the chrome showed when it began, whether it can still
    // be a tap (it began off the chrome and stayed within the slop), and where it began.
    bool pressShown_ = false;
    bool tap_ = false;
    float downX_ = 0.0f, downY_ = 0.0f;
};

} // namespace mp::ui
