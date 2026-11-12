// SPDX-License-Identifier: Apache-2.0
#include "ui/TapHandler.h"

namespace mp::ui {

bool TapHandler::Press(float x, float y, double t, bool onChrome) {
    pressShown_ = Shown(t);
    tap_ = !(pressShown_ && onChrome);
    downX_ = x;
    downY_ = y;
    lastTouch_ = t;
    return pressShown_;
}

void TapHandler::Move(float x, float y, double t) {
    const float dx = x - downX_, dy = y - downY_;
    if (dx * dx + dy * dy > slop_ * slop_) tap_ = false;
    lastTouch_ = t;
}

void TapHandler::Release(float x, float y, double t) {
    Move(x, y, t);
    if (tap_) hidden_ = pressShown_;
    tap_ = false;
}

void TapHandler::Cancel(double t) {
    tap_ = false;
    lastTouch_ = t;
}

void TapHandler::Show(double t) {
    hidden_ = false;
    Wake(t);
}

bool TapHandler::Shown(double t) const {
    return !hidden_ && (held_ || t - lastTouch_ < kShowSeconds);
}

} // namespace mp::ui
