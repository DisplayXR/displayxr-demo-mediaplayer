// SPDX-License-Identifier: Apache-2.0
//
// SlideGesture: swiping from one file to the next in the player. The picture follows a
// horizontal drag with the neighbour's page beside it. A release past the swipe distance
// slides the picture out and the neighbour in, a shorter one slides back, and with no
// neighbour that way the picture gives a little and slides back. When a slide out is
// over, Update() asks for the neighbour to be opened, and the page stays put until
// Reset(). Pure logic, render thread. Times in seconds, positions in pixels, offsets in
// page widths.
#pragma once

namespace mp::browser {

class SlideGesture {
public:
    // The page width, how far a press moves before it drags the picture, and how far a
    // drag goes before its release changes the file.
    void SetMetrics(float width, float slop, float swipe);
    // Whether there is a file before and after the one shown.
    void SetNeighbours(bool previous, bool next);

    void Press(float x, float y);
    void Move(float x, float y);
    void Release(double t);
    // The press is not ours (it began on the chrome, or the system took it): let go.
    void Cancel(double t);

    // Advance the slide to `t`. Returns +1 (next) or -1 (previous), once, when a slide
    // out is over and that file should be opened; 0 otherwise.
    int Update(double t);
    // The opened file has replaced the picture, or the picture is gone: back to rest.
    void Reset();

    // The picture's offset, + = right.
    float Offset() const { return offset_; }
    // The neighbour whose page is beside the picture, at Offset() + Neighbour(): +1 the
    // next file (on the right), -1 the previous one (on the left), 0 none.
    int Neighbour() const;
    // A file was asked for and Reset() has not come yet.
    bool Waiting() const { return state_ == State::Waiting; }
    // Sliding or waiting: a press now would be ignored, so the caller holds it back.
    bool Busy() const { return state_ == State::Sliding || state_ == State::Waiting; }
    // A press is held back: end the slide at the next Update().
    void Hurry();

private:
    enum class State { Idle, Pressed, Dragging, Sliding, Waiting };
    static constexpr double kSlideSeconds = 0.25;
    static constexpr float kGive = 0.25f;  // of the finger's travel, with no neighbour that way

    float Follow(float dx) const;
    void SlideTo(float to, int step, double t);

    float width_ = 1.0f, slop_ = 0.0f, swipe_ = 0.0f;
    bool previous_ = false, next_ = false;

    State state_ = State::Idle;
    float pressX_ = 0.0f, pressY_ = 0.0f;
    float dx_ = 0.0f, dy_ = 0.0f;  // the finger's travel since the press
    float offset_ = 0.0f;
    float from_ = 0.0f, to_ = 0.0f;  // the slide
    double t0_ = 0.0;
    int step_ = 0;  // the file the slide opens, 0 for a slide back
};

} // namespace mp::browser
