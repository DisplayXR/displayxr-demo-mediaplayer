// SPDX-License-Identifier: Apache-2.0
#include "browser/SlideGesture.h"

#include <cmath>

namespace mp::browser {

void SlideGesture::SetMetrics(float width, float slop, float swipe) {
    width_ = width > 0.0f ? width : 1.0f;
    slop_ = slop;
    swipe_ = swipe;
}

void SlideGesture::SetNeighbours(bool previous, bool next) {
    previous_ = previous;
    next_ = next;
}

void SlideGesture::Press(float x, float y) {
    if (state_ != State::Idle) return;  // a slide or an open runs its course
    state_ = State::Pressed;
    pressX_ = x;
    pressY_ = y;
    dx_ = dy_ = 0.0f;
}

void SlideGesture::Move(float x, float y) {
    if (state_ != State::Pressed && state_ != State::Dragging) return;
    dx_ = x - pressX_;
    dy_ = y - pressY_;
    if (state_ == State::Pressed && std::fabs(dx_) >= slop_ && std::fabs(dx_) > std::fabs(dy_))
        state_ = State::Dragging;
    if (state_ == State::Dragging) offset_ = Follow(dx_);
}

void SlideGesture::Release(double t) {
    if (state_ == State::Pressed) state_ = State::Idle;
    if (state_ != State::Dragging) return;
    const int step = dx_ < 0.0f ? 1 : -1;
    const bool there = step > 0 ? next_ : previous_;
    if (there && std::fabs(dx_) >= swipe_ && std::fabs(dx_) > 2.0f * std::fabs(dy_))
        SlideTo((float)-step, step, t);
    else
        SlideTo(0.0f, 0, t);
}

void SlideGesture::Cancel(double t) {
    if (state_ == State::Pressed) state_ = State::Idle;
    if (state_ == State::Dragging) SlideTo(0.0f, 0, t);
}

void SlideGesture::Hurry() {
    if (state_ == State::Sliding) t0_ = -kSlideSeconds;
}

int SlideGesture::Update(double t) {
    if (state_ != State::Sliding) return 0;
    const float u = (float)std::fmin(1.0, (t - t0_) / kSlideSeconds);
    const float left = 1.0f - u;
    offset_ = from_ + (to_ - from_) * (1.0f - left * left * left);  // ease out
    if (u < 1.0f) return 0;
    offset_ = to_;
    state_ = step_ != 0 ? State::Waiting : State::Idle;
    return step_;
}

void SlideGesture::Reset() {
    state_ = State::Idle;
    offset_ = 0.0f;
    step_ = 0;
}

int SlideGesture::Neighbour() const {
    if (step_ != 0 && (state_ == State::Sliding || state_ == State::Waiting)) return step_;
    if (offset_ < 0.0f && next_) return 1;
    if (offset_ > 0.0f && previous_) return -1;
    return 0;
}

float SlideGesture::Follow(float dx) const {
    const float offset = dx / width_;
    const bool there = offset < 0.0f ? next_ : previous_;
    return there ? offset : offset * kGive;
}

void SlideGesture::SlideTo(float to, int step, double t) {
    state_ = State::Sliding;
    from_ = offset_;
    to_ = to;
    step_ = step;
    t0_ = t;
}

} // namespace mp::browser
