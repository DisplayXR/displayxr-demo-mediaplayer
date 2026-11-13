// SPDX-License-Identifier: Apache-2.0
#include "pro/LiveSourceBase.h"

#include "media/V210.h"
#include "pro/ConvertPool.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace mp {

LiveSourceBase::LiveSourceBase() {
    int depth = 3;
    if (const char* q = std::getenv("MEDIAPLAYER_LIVE_QUEUE")) depth = std::atoi(q);
    ring_.SetQueueDepth(depth);
}

std::string LiveSourceBase::LastError() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return lastError_;
}

LiveStats LiveSourceBase::GetStats() const {
    LiveStats s;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        s = stats_;
    }
    // Queue mode: a standing backlog trimmed on the consumer side is a drop too.
    s.dropped += ring_.TrimmedFrames() - trimmedAtReset_;
    s.repeated = ring_.RepeatedFrames() - repeatedAtReset_;
    s.liveQueue = ring_.QueueMode();
    return s;
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
    trimmedAtReset_ = ring_.TrimmedFrames();
    repeatedAtReset_ = ring_.RepeatedFrames();
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
    f.eyes = 1;
    f.ClearExt();
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
                             const char* timecode, int eyeOffsetFrames, livepix::ConvertPool* pool) {
    if (src.width <= 0 || src.height <= 0) return;
    FrameRing::Frame& f = ring_.WriteBuffer();
    f.plane[0].resize(src.y.size() * 2);  // same size after warm-up: no allocation
    f.plane[1].resize(src.uv.size() * 2);
    // (a dual-stream 4K pair is 50 MB: row bands on the caller's convert threads)
    livepix::CopyFrameBanded(pool, src, reinterpret_cast<uint16_t*>(f.plane[0].data()),
                             reinterpret_cast<uint16_t*>(f.plane[1].data()));
    f.width = src.width;
    f.height = src.height;
    f.format = PixFormat::P016;
    f.eyes = 1;
    f.ClearExt();
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

void LiveSourceBase::SizePairPlanes(FrameRing::Frame& f, int w, int h) {
    f.plane[0].resize((size_t)w * h * 2);  // same size after warm-up: no allocation
    f.plane[1].resize((size_t)w * (h / 2) * 2);
}

void LiveSourceBase::TagPair(FrameRing::Frame& f, int w, int h, ColorMatrix matrix, ColorTransfer transfer,
                             bool fullRange, double captureSec, const char* timecode) {
    f.width = w;
    f.height = h;
    f.format = PixFormat::P016;
    f.eyes = 1;
    f.ClearExt();
    f.fullRange = fullRange;
    f.gpu = false;
    f.gpuSharedTexture = nullptr;
    f.gpuSharedHandle = nullptr;
    f.captureSec = captureSec;
    f.matrix = matrix;
    f.transfer = transfer;
    std::memset(f.timecode, 0, sizeof(f.timecode));
    if (timecode) std::strncpy(f.timecode, timecode, sizeof(f.timecode) - 1);
}

void LiveSourceBase::PublishPair(const livepix::P016& l, const livepix::P016& r, bool fullRange,
                                 double captureSec, const char* timecode, int eyeOffsetFrames,
                                 livepix::ConvertPool* pool) {
    PublishPair(livepix::EyeOf(l), livepix::EyeOf(r), l.matrix, l.transfer, fullRange, captureSec, timecode,
                eyeOffsetFrames, pool);
}

void LiveSourceBase::PublishPair(const livepix::EyeView& l, const livepix::EyeView& r, ColorMatrix matrix,
                                 ColorTransfer transfer, bool fullRange, double captureSec, const char* timecode,
                                 int eyeOffsetFrames, livepix::ConvertPool* pool) {
    const int cw = std::min(l.width, r.width), ch = std::min(l.height, r.height) & ~1;
    if (cw <= 0 || ch <= 0) return;
    const int w = 2 * cw;
    FrameRing::Frame& f = ring_.WriteBuffer();
    SizePairPlanes(f, w, ch);
    // Row bands on the caller's convert threads: ~10 ms for a 4K pair on one thread.
    livepix::ComposePairBanded(pool, l, r, cw, ch, reinterpret_cast<uint16_t*>(f.plane[0].data()),
                               reinterpret_cast<uint16_t*>(f.plane[1].data()));
    TagPair(f, w, ch, matrix, transfer, fullRange, captureSec, timecode);
    Published(w, ch, fullRange, eyeOffsetFrames, ring_.Publish());
}

FrameRing::Frame* LiveSourceBase::ReservePairFrame(int eyeW, int eyeH) {
    if (eyeW <= 0 || eyeH <= 0 || (eyeH & 1)) return nullptr;
    FrameRing::Frame* f = ring_.ReserveBuffer();
    if (f) SizePairPlanes(*f, 2 * eyeW, eyeH);  // a reused buffer: no allocation after warm-up
    return f;
}

void LiveSourceBase::CancelPairFrame(FrameRing::Frame* f) {
    if (f) ring_.CancelReserved(f);
}

void LiveSourceBase::PublishPairFrame(FrameRing::Frame* f, int eyeW, int eyeH, ColorMatrix matrix,
                                      ColorTransfer transfer, bool fullRange, double captureSec,
                                      const char* timecode, int eyeOffsetFrames) {
    if (!f) return;
    const int w = 2 * eyeW;
    TagPair(*f, w, eyeH, matrix, transfer, fullRange, captureSec, timecode);
    Published(w, eyeH, fullRange, eyeOffsetFrames, ring_.PublishReserved(f));
}

bool LiveSourceBase::PublishV210(std::vector<uint8_t>* const eye[2], int eyes, int eyeW, int h, ColorMatrix matrix,
                                 ColorTransfer transfer, bool fullRange, double captureSec, const char* timecode,
                                 int eyeOffsetFrames) {
    if ((eyes != 1 && eyes != 2) || eyeW <= 0 || (eyeW & 1) || h <= 0 || (h & 1)) return false;
    const size_t bytes = v210::RowBytes(eyeW) * (size_t)h;
    for (int e = 0; e < eyes; ++e)
        if (!eye[e] || eye[e]->size() < bytes) return false;
    FrameRing::Frame& f = ring_.WriteBuffer();
    for (int e = 0; e < eyes; ++e) f.plane[e].swap(*eye[e]);  // a move: no pixel copy
    for (int e = eyes; e < 3; ++e) f.plane[e].clear();
    f.width = eyes * eyeW;
    f.height = h;
    f.format = PixFormat::V210;
    f.eyes = eyes;
    f.ClearExt();
    f.fullRange = fullRange;
    f.gpu = false;
    f.gpuSharedTexture = nullptr;
    f.gpuSharedHandle = nullptr;
    f.captureSec = captureSec;
    f.matrix = matrix;
    f.transfer = transfer;
    std::memset(f.timecode, 0, sizeof(f.timecode));
    if (timecode) std::strncpy(f.timecode, timecode, sizeof(f.timecode) - 1);
    Published(eyes * eyeW, h, fullRange, eyeOffsetFrames, ring_.Publish());
    return true;
}

bool LiveSourceBase::PublishV210Ext(const FrameRing::Frame::ExtPlane* const eye[2], int eyes, int eyeW, int h,
                                    ColorMatrix matrix, ColorTransfer transfer, bool fullRange, double captureSec,
                                    const char* timecode, int eyeOffsetFrames) {
    if ((eyes != 1 && eyes != 2) || eyeW <= 0 || (eyeW & 1) || h <= 0 || (h & 1)) return false;
    const size_t bytes = v210::RowBytes(eyeW) * (size_t)h;
    for (int e = 0; e < eyes; ++e)
        if (!eye[e] || !eye[e]->data || eye[e]->bytes < bytes || !eye[e]->block || !eye[e]->hold) return false;
    FrameRing::Frame& f = ring_.WriteBuffer();
    f.ClearExt();
    for (int e = 0; e < eyes; ++e) {
        f.ext[e] = *eye[e];
        f.plane[e].clear();  // (capacity kept for a later plane[] publish)
    }
    f.plane[2].clear();
    f.width = eyes * eyeW;
    f.height = h;
    f.format = PixFormat::V210;
    f.eyes = eyes;
    f.fullRange = fullRange;
    f.gpu = false;
    f.gpuSharedTexture = nullptr;
    f.gpuSharedHandle = nullptr;
    f.captureSec = captureSec;
    f.matrix = matrix;
    f.transfer = transfer;
    std::memset(f.timecode, 0, sizeof(f.timecode));
    if (timecode) std::strncpy(f.timecode, timecode, sizeof(f.timecode) - 1);
    Published(eyes * eyeW, h, fullRange, eyeOffsetFrames, ring_.Publish());
    return true;
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
    LiveState st = state_.load();
    // compare-exchange: a source may go Failed on another thread (a DeckLink restart that
    // failed, #169) between the load and the store, and Failed must not become Stalled.
    if ((st == LiveState::Streaming || st == LiveState::Opening) && last > 0.0 && NowSeconds() - last > stallSec)
        state_.compare_exchange_strong(st, LiveState::Stalled);
}

}  // namespace mp
