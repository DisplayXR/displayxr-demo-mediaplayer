// SPDX-License-Identifier: Apache-2.0
//
// ZoomGesture: pinch and double-tap zoom on a still in the touch player (#180). A double
// tap toggles fit and 2x round the tapped point, a pinch zooms round its midpoint from fit
// to 4x, and one finger pans while zoomed. The picture never pans past its own edges, and
// at fit it is centred. The caller applies the same zoom and offset to both views, so the
// depth is kept. Swiping to the next file belongs to fit zoom only: while Zoomed() or
// Active(), the caller keeps the touch from the swipe. Pure logic, render thread. Times in
// seconds, positions in window pixels.
#pragma once

namespace mp::ui {

class ZoomGesture {
public:
    static constexpr float kMaxZoom = 4.0f;
    static constexpr float kTapZoom = 2.0f;

    // The window, and the picture's size in it at fit (it may overflow on one axis).
    void SetView(float width, float height, float fitWidth, float fitHeight);
    // How far a press may move and still be a tap.
    void SetSlop(float px) { slop_ = px; }
    // Back to fit, forgetting any gesture (a new file).
    void Reset();

    // A touch, as MotionEvent reports it: action 0 down, 1 up, 2 move, 3 cancel, 5 a second
    // finger down, 6 a finger up of several. `pointers` fingers are down; (x1, y1) is the
    // second when there are two.
    void Touch(int action, float x, float y, int pointers, float x1, float y1, double t);

    // A double tap toggles fit and kTapZoom round (x, y).
    void DoubleTap(float x, float y);

    float Zoom() const { return zoom_; }
    // The picture's centre from the window's, pixels.
    float OffsetX() const { return cx_; }
    float OffsetY() const { return cy_; }
    bool Zoomed() const { return zoom_ > 1.0f + 1e-3f; }
    // A pinch or a pan is under way.
    bool Active() const { return pinching_ || panning_; }

private:
    static constexpr double kTapSeconds = 0.30;     // a press this short can be a tap
    static constexpr double kDoubleSeconds = 0.35;  // the second tap within this of the first

    void ZoomAbout(float z, float fx, float fy);  // to `z`, keeping the point under (fx, fy)
    void Clamp();

    float w_ = 1.0f, h_ = 1.0f, fitW_ = 1.0f, fitH_ = 1.0f;
    float slop_ = 12.0f;
    float zoom_ = 1.0f, cx_ = 0.0f, cy_ = 0.0f;

    // The press under way.
    bool tap_ = false;  // can still be a tap
    float downX_ = 0.0f, downY_ = 0.0f;
    double downT_ = 0.0;
    bool panning_ = false;
    float lastX_ = 0.0f, lastY_ = 0.0f;
    // The pinch under way: its start.
    bool pinching_ = false;
    float pinchD0_ = 1.0f, pinchZ0_ = 1.0f, pinchMx0_ = 0.0f, pinchMy0_ = 0.0f, pinchCx0_ = 0.0f, pinchCy0_ = 0.0f;
    // The last tap, for a double tap.
    bool tapped_ = false;
    float tapX_ = 0.0f, tapY_ = 0.0f;
    double tapT_ = 0.0;
};

}  // namespace mp::ui
