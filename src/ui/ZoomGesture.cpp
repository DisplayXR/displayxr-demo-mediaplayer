// SPDX-License-Identifier: Apache-2.0
#include "ui/ZoomGesture.h"

#include <algorithm>
#include <cmath>

namespace mp::ui {

void ZoomGesture::SetView(float width, float height, float fitWidth, float fitHeight) {
    if (!(width > 0.0f) || !(height > 0.0f) || !(fitWidth > 0.0f) || !(fitHeight > 0.0f)) return;
    w_ = width;
    h_ = height;
    fitW_ = fitWidth;
    fitH_ = fitHeight;
    Clamp();
}

void ZoomGesture::Reset() {
    zoom_ = 1.0f;
    cx_ = cy_ = 0.0f;
    tap_ = panning_ = pinching_ = tapped_ = false;
}

void ZoomGesture::Clamp() {
    zoom_ = std::min(kMaxZoom, std::max(1.0f, zoom_));
    if (!Zoomed()) {
        zoom_ = 1.0f;
        cx_ = cy_ = 0.0f;
        return;
    }
    // The picture may move only as far as it overflows the window, on each axis.
    const float mx = std::max(0.0f, (fitW_ * zoom_ - w_) * 0.5f);
    const float my = std::max(0.0f, (fitH_ * zoom_ - h_) * 0.5f);
    cx_ = std::min(mx, std::max(-mx, cx_));
    cy_ = std::min(my, std::max(-my, cy_));
}

void ZoomGesture::ZoomAbout(float z, float fx, float fy) {
    // The picture's point under (fx, fy), from the window's centre, stays under it.
    const float px = fx - w_ * 0.5f, py = fy - h_ * 0.5f;
    z = std::min(kMaxZoom, std::max(1.0f, z));
    cx_ = px - (px - cx_) * z / zoom_;
    cy_ = py - (py - cy_) * z / zoom_;
    zoom_ = z;
    Clamp();
}

void ZoomGesture::DoubleTap(float x, float y) {
    if (Zoomed()) {
        zoom_ = 1.0f;
        Clamp();
    } else {
        ZoomAbout(kTapZoom, x, y);
    }
}

void ZoomGesture::Touch(int action, float x, float y, int pointers, float x1, float y1, double t) {
    switch (action) {
        case 0:  // the first finger down
            tap_ = true;
            downX_ = lastX_ = x;
            downY_ = lastY_ = y;
            downT_ = t;
            panning_ = Zoomed();
            pinching_ = false;
            break;
        case 5:  // another finger: a pinch
            if (pointers >= 2) {
                tap_ = panning_ = false;
                pinching_ = true;
                pinchD0_ = std::max(1.0f, std::hypot(x1 - x, y1 - y));
                pinchZ0_ = zoom_;
                pinchMx0_ = (x + x1) * 0.5f - w_ * 0.5f;
                pinchMy0_ = (y + y1) * 0.5f - h_ * 0.5f;
                pinchCx0_ = cx_;
                pinchCy0_ = cy_;
            }
            break;
        case 2:  // a move
            if (pinching_ && pointers >= 2) {
                const float z = std::min(kMaxZoom, std::max(1.0f, pinchZ0_ * std::hypot(x1 - x, y1 - y) / pinchD0_));
                // The pinch's start point follows its midpoint, at the new zoom.
                const float mx = (x + x1) * 0.5f - w_ * 0.5f, my = (y + y1) * 0.5f - h_ * 0.5f;
                cx_ = mx - (pinchMx0_ - pinchCx0_) * z / pinchZ0_;
                cy_ = my - (pinchMy0_ - pinchCy0_) * z / pinchZ0_;
                zoom_ = z;
                Clamp();
            } else if (!pinching_) {
                if (std::fabs(x - downX_) > slop_ || std::fabs(y - downY_) > slop_) tap_ = false;
                if (panning_) {
                    cx_ += x - lastX_;
                    cy_ += y - lastY_;
                    Clamp();
                }
                lastX_ = x;
                lastY_ = y;
            }
            break;
        case 6:  // a finger up of several: the pinch is over, the rest waits for a new press
            pinching_ = false;
            panning_ = false;
            break;
        case 1: {  // the last finger up
            const bool tap = tap_ && !pinching_ && t - downT_ <= kTapSeconds;
            tap_ = panning_ = pinching_ = false;
            if (!tap) {
                tapped_ = false;
                break;
            }
            if (tapped_ && t - tapT_ <= kDoubleSeconds && std::fabs(x - tapX_) <= slop_ * 3.0f &&
                std::fabs(y - tapY_) <= slop_ * 3.0f) {
                tapped_ = false;
                DoubleTap(x, y);
            } else {
                tapped_ = true;
                tapX_ = x;
                tapY_ = y;
                tapT_ = t;
            }
            break;
        }
        default:  // a cancel
            tap_ = panning_ = pinching_ = tapped_ = false;
            break;
    }
}

}  // namespace mp::ui
