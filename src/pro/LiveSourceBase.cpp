// SPDX-License-Identifier: Apache-2.0
#include "pro/LiveSourceBase.h"

#include <algorithm>
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
    openedAt_ = NowSeconds();
    fpsWindowStart_ = openedAt_;
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
    f.matrix = src.matrix;
    f.transfer = src.transfer;
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
    f.matrix = src.matrix;
    f.transfer = src.transfer;
    std::memset(f.timecode, 0, sizeof(f.timecode));
    if (timecode) std::strncpy(f.timecode, timecode, sizeof(f.timecode) - 1);
    Published(src.width, src.height, fullRange, eyeOffsetFrames, ring_.Publish());
}

void LiveSourceBase::PublishPair(const livepix::P016& l, const livepix::P016& r, bool fullRange,
                                 double captureSec, const char* timecode, int eyeOffsetFrames) {
    const int cw = std::min(l.width, r.width), ch = std::min(l.height, r.height) & ~1;
    if (cw <= 0 || ch <= 0) return;
    const int w = 2 * cw;
    FrameRing::Frame& f = ring_.WriteBuffer();
    f.plane[0].resize((size_t)w * ch * 2);  // same size after warm-up: no allocation
    f.plane[1].resize((size_t)w * (ch / 2) * 2);
    uint16_t* y = reinterpret_cast<uint16_t*>(f.plane[0].data());
    uint16_t* uv = reinterpret_cast<uint16_t*>(f.plane[1].data());
    const size_t eyeBytes = (size_t)cw * 2;
    for (int row = 0; row < ch; ++row) {
        std::memcpy(y + (size_t)row * w, l.y.data() + (size_t)row * l.width, eyeBytes);
        std::memcpy(y + (size_t)row * w + cw, r.y.data() + (size_t)row * r.width, eyeBytes);
    }
    for (int row = 0; row < ch / 2; ++row) {
        std::memcpy(uv + (size_t)row * w, l.uv.data() + (size_t)row * l.width, eyeBytes);
        std::memcpy(uv + (size_t)row * w + cw, r.uv.data() + (size_t)row * r.width, eyeBytes);
    }
    f.width = w;
    f.height = ch;
    f.format = PixFormat::P016;
    f.fullRange = fullRange;
    f.gpu = false;
    f.gpuSharedTexture = nullptr;
    f.gpuSharedHandle = nullptr;
    f.captureSec = captureSec;
    f.matrix = l.matrix;
    f.transfer = l.transfer;
    std::memset(f.timecode, 0, sizeof(f.timecode));
    if (timecode) std::strncpy(f.timecode, timecode, sizeof(f.timecode) - 1);
    Published(w, ch, fullRange, eyeOffsetFrames, ring_.Publish());
}

void LiveSourceBase::CountLate() {
    std::lock_guard<std::mutex> lk(statsMutex_);
    ++stats_.lateFrames;
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

void LiveSourceBase::SetNoSignal(int eye, bool on) {
    std::lock_guard<std::mutex> lk(statsMutex_);
    const int bit = 1 << (eye & 1);
    stats_.noSignalEyes = on ? (stats_.noSignalEyes | bit) : (stats_.noSignalEyes & ~bit);
}

void LiveSourceBase::CheckStall(double stallSec) {
    double last;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        last = stats_.lastPublishSec > 0.0 ? stats_.lastPublishSec : openedAt_;
    }
    const LiveState st = state_.load();
    if ((st == LiveState::Streaming || st == LiveState::Opening) && last > 0.0 && NowSeconds() - last > stallSec)
        state_ = LiveState::Stalled;
}

}  // namespace mp
