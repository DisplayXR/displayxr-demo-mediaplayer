// SPDX-License-Identifier: Apache-2.0
#include "browser/TouchScroller.h"

#include <algorithm>
#include <cmath>

namespace mp::browser {

float TouchScroller::Clamp(float offset) const { return std::min(std::max(offset, 0.0f), limit_); }

void TouchScroller::SetLimit(float limit) {
    limit_ = std::max(limit, 0.0f);
    offset_ = Clamp(offset_);
}

void TouchScroller::SetOffset(float offset) {
    offset_ = Clamp(offset);
    flinging_ = false;
}

float TouchScroller::FlingLeft(double t) const {
    return std::fabs(flingSpeed_ * (float)(kFlingTau * std::exp(-(t - flingT0_) / kFlingTau)));
}

void TouchScroller::Press(float x, float y, double t) {
    pressed_ = true;
    dragging_ = false;
    // A press that stops a fling is not a tap, unless less than the slop was left to
    // travel: the fling looked stopped.
    tap_ = !flinging_ || FlingLeft(t) < slop_;
    flinging_ = false;
    pressX_ = x;
    pressY_ = y;
    lastY_ = y;
    samples_.assign(1, Sample{y, t});
}

void TouchScroller::Move(float x, float y, double t) {
    if (!pressed_) return;
    const float dx = x - pressX_, dy = y - pressY_;
    if (dx * dx + dy * dy > slop_ * slop_) tap_ = false;
    if (!dragging_ && std::fabs(dy) > slop_) {
        // Start from the slop's edge, so the content neither jumps nor loses the motion
        // past the slop.
        dragging_ = true;
        lastY_ = pressY_ + (dy > 0.0f ? slop_ : -slop_);
    }
    if (dragging_) {
        offset_ = Clamp(offset_ + (lastY_ - y));
        lastY_ = y;
    }
    samples_.push_back(Sample{y, t});
    while (samples_.size() > 2 && samples_.front().t < t - kVelocityWindow) samples_.erase(samples_.begin());
}

bool TouchScroller::Release(double t) {
    if (!pressed_) return false;
    pressed_ = false;
    if (dragging_) {
        dragging_ = false;
        // The speed over the drag's last moments: a finger that stopped before lifting
        // left no samples in the window, so it does not fling.
        while (!samples_.empty() && samples_.front().t < t - kVelocityWindow) samples_.erase(samples_.begin());
        if (samples_.size() >= 2) {
            const Sample& a = samples_.front();
            const Sample& b = samples_.back();
            const double span = b.t - a.t;
            const float speed = span > 1e-3 ? (float)((a.y - b.y) / span) : 0.0f;
            // Only a fling that would travel further than the slop is worth starting.
            if (std::fabs(speed) * (float)kFlingTau > slop_) {
                flinging_ = true;
                flingT0_ = t;
                flingOffset0_ = offset_;
                flingSpeed_ = speed;
            }
        }
    }
    samples_.clear();
    return tap_;
}

void TouchScroller::Update(double t) {
    if (!flinging_) return;
    // Speed decays exponentially, so the distance is solved in closed form and does not
    // depend on the frame rate: it tends to speed * tau.
    const double decay = std::exp(-(t - flingT0_) / kFlingTau);
    const float target = flingOffset0_ + (float)(flingSpeed_ * kFlingTau * (1.0 - decay));
    offset_ = Clamp(target);
    // Stop at an end, or once less than half a pixel of travel remains.
    if (offset_ != target || FlingLeft(t) < 0.5f) flinging_ = false;
}

} // namespace mp::browser
