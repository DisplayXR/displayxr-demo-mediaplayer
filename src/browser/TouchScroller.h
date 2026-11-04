// SPDX-License-Identifier: Apache-2.0
//
// TouchScroller: the vertical scroll offset of one list under a finger or a mouse. ImGui
// has no touch scrolling, so this does it: a drag past the slop scrolls, a fling keeps
// going and slows to a stop, a press that stops a fling only stops it, and a press that
// never moved past the slop is a tap. Pure logic, render thread. Times in seconds,
// distances in pixels; the offset grows as the content moves up.
#pragma once

#include <vector>

namespace mp::browser {

class TouchScroller {
public:
    // How far a press may move and still be a tap.
    void SetSlop(float px) { slop_ = px; }
    // The largest offset: content height minus view height, 0 when it fits. Clamps the offset.
    void SetLimit(float limit);
    // Jump to `offset` and stop any fling.
    void SetOffset(float offset);
    float Offset() const { return offset_; }

    void Press(float x, float y, double t);
    void Move(float x, float y, double t);
    // True if the press was a tap.
    bool Release(double t);
    // Advance a fling to `t`.
    void Update(double t);

    bool Pressed() const { return pressed_; }
    bool Dragging() const { return dragging_; }

private:
    static constexpr double kVelocityWindow = 0.1;  // the last 0.1 s of a drag sets the fling's speed
    static constexpr double kFlingTau = 0.4;        // a fling's speed falls by a factor e every 0.4 s

    struct Sample {
        float y;
        double t;
    };

    float Clamp(float offset) const;

    float slop_ = 0.0f;
    float limit_ = 0.0f;
    float offset_ = 0.0f;

    bool pressed_ = false;
    bool dragging_ = false;
    bool tap_ = false;  // the press has stayed within the slop and did not stop a fling
    float pressX_ = 0.0f, pressY_ = 0.0f, lastY_ = 0.0f;
    std::vector<Sample> samples_;  // the drag's recent positions, for the fling speed

    bool flinging_ = false;
    double flingT0_ = 0.0;
    float flingOffset0_ = 0.0f;
    float flingSpeed_ = 0.0f;  // px/s at flingT0_, in offset terms
};

} // namespace mp::browser
