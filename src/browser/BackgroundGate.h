// SPDX-License-Identifier: Apache-2.0
//
// BackgroundGate (#267): capture comes first. Media Player Pro may be capturing two SDI
// inputs while someone browses; the browser's thumbnail work (the shell's decoders, a
// video frame grab, the 3D classifier) then stops. It pauses as soon as a capture streams
// and resumes only once the capture has been off for `resumeAfterS`, so a signal that
// drops for a moment does not restart the thumbnail work each time. Pure; the caller
// passes the clock. tests/pro_browser_test.cpp.
#pragma once

namespace mp::browser {

class BackgroundGate {
public:
    explicit BackgroundGate(double resumeAfterS = 2.0) : resumeAfterS_(resumeAfterS) {}

    // Once a frame. True when Paused() changed with this call.
    bool Update(bool captureActive, double nowS) {
        const bool was = paused_;
        if (captureActive) {
            paused_ = true;
            offSince_ = -1.0;
        } else if (paused_) {
            if (offSince_ < 0.0) offSince_ = nowS;
            if (nowS - offSince_ >= resumeAfterS_) paused_ = false;
        }
        return paused_ != was;
    }
    bool Paused() const { return paused_; }

    // The one line the browser shows while paused.
    static const char* Notice() { return "Thumbnails paused while capture runs."; }

private:
    double resumeAfterS_;
    bool paused_ = false;
    double offSince_ = -1.0;
};

}  // namespace mp::browser
