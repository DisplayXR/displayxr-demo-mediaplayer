// SPDX-License-Identifier: Apache-2.0
#include "pro/LiveSourceBase.h"

#include <cstring>

namespace mp {

std::string LiveSourceBase::LastError() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return lastError_;
}

LiveStats LiveSourceBase::GetStats() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return stats_;
}

void LiveSourceBase::Fail(LiveState s, const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        lastError_ = why;
    }
    state_ = s;
}

void LiveSourceBase::ResetStats(int fpsReq, double fps) {
    std::lock_guard<std::mutex> lk(statsMutex_);
    stats_ = LiveStats{};
    stats_.fpsReq = fpsReq;
    stats_.fps = fps;
    lastError_.clear();
    fpsWindowStart_ = NowSeconds();
    fpsWindowCount_ = 0;
}

void LiveSourceBase::SetFps(double fps) {
    std::lock_guard<std::mutex> lk(statsMutex_);
    stats_.fps = fps;
    stats_.fpsReq = (int)(fps + 0.5);
}

void LiveSourceBase::Publish(const livepix::Nv12& src, bool fullRange, double captureSec,
                             const char* timecode, int eyeOffsetFrames) {
    if (src.width <= 0 || src.height <= 0) return;
    FrameRing::Frame& f = ring_.WriteBuffer();
    f.plane[0].assign(src.y.begin(), src.y.end());  // same size after warm-up: no allocation
    f.plane[1].assign(src.uv.begin(), src.uv.end());
    f.width = src.width;
    f.height = src.height;
    f.format = PixFormat::NV12;
    f.fullRange = fullRange;
    f.gpu = false;
    f.gpuSharedTexture = nullptr;
    f.gpuSharedHandle = nullptr;
    f.captureSec = captureSec;
    std::memset(f.timecode, 0, sizeof(f.timecode));
    if (timecode) std::strncpy(f.timecode, timecode, sizeof(f.timecode) - 1);
    Published(src.width, src.height, fullRange, eyeOffsetFrames, ring_.Publish());
}

void LiveSourceBase::Publish(const livepix::P016& src, bool fullRange, double captureSec,
                             const char* timecode, int eyeOffsetFrames) {
    if (src.width <= 0 || src.height <= 0) return;
    FrameRing::Frame& f = ring_.WriteBuffer();
    const uint8_t* yb = reinterpret_cast<const uint8_t*>(src.y.data());
    const uint8_t* uvb = reinterpret_cast<const uint8_t*>(src.uv.data());
    f.plane[0].assign(yb, yb + src.y.size() * 2);
    f.plane[1].assign(uvb, uvb + src.uv.size() * 2);
    f.width = src.width;
    f.height = src.height;
    f.format = PixFormat::P016;
    f.fullRange = fullRange;
    f.gpu = false;
    f.gpuSharedTexture = nullptr;
    f.gpuSharedHandle = nullptr;
    f.captureSec = captureSec;
    std::memset(f.timecode, 0, sizeof(f.timecode));
    if (timecode) std::strncpy(f.timecode, timecode, sizeof(f.timecode) - 1);
    Published(src.width, src.height, fullRange, eyeOffsetFrames, ring_.Publish());
}

void LiveSourceBase::Published(int w, int h, bool fullRange, int eyeOffsetFrames, bool dropped) {
    const double now = NowSeconds();
    ++fpsWindowCount_;
    std::lock_guard<std::mutex> lk(statsMutex_);
    ++stats_.published;
    if (dropped) ++stats_.dropped;
    stats_.lastPublishSec = now;
    stats_.w = w;
    stats_.h = h;
    stats_.fullRange = fullRange;
    stats_.eyeOffsetFrames = eyeOffsetFrames;
    const double el = now - fpsWindowStart_;
    if (el >= 1.0) {
        stats_.deliveredFps = (float)((double)fpsWindowCount_ / el);
        fpsWindowCount_ = 0;
        fpsWindowStart_ = now;
    }
    if (state_ == LiveState::Opening || state_ == LiveState::Stalled) state_ = LiveState::Streaming;
}

void LiveSourceBase::CheckStall(double stallSec) {
    double last;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        last = stats_.lastPublishSec;
    }
    const LiveState st = state_.load();
    if (st == LiveState::Streaming && last > 0.0 && NowSeconds() - last > stallSec)
        state_ = LiveState::Stalled;
}

}  // namespace mp
