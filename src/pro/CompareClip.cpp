// SPDX-License-Identifier: Apache-2.0
#include "pro/CompareClip.h"

#include "Log.h"
#include "media/ImageDecoder.h"
#include "media/MediaSource.h"
#include "rhi/VulkanRenderer.h"

#include <cmath>

namespace mp::pro {

bool CompareClip::Open(const std::string& path, std::string* err) {
    Close();
    const MediaInfo info = MediaSource::Identify(path);
    if (info.kind == MediaKind::Image) {
        DecodedImage img = ImageDecoder::Load(path);
        if (!img.Valid()) {
            if (err) *err = "B is not an image this player decodes";
            return false;
        }
        still_ = std::move(img.pixels);
        stillW_ = img.width;
        stillH_ = img.height;
        isStill_ = true;
    } else {
        video_ = std::make_unique<VideoDecoder>();
        if (!video_->Open(path)) {
            video_.reset();
            if (err) *err = "B does not open as a video";
            return false;
        }
        video_->SetLoop(true);  // B follows A anyway; at its end it waits for the next seek
        isStill_ = false;
    }
    path_ = path;
    open_ = true;
    stillUploaded_ = false;
    lastSeekSec_ = -10.0;
    lastSeekTo_ = -1.0;
    LOG_INFO("Pro: compare clip B = %s (%s)", path.c_str(), isStill_ ? "still" : "video");
    return true;
}

void CompareClip::Close() {
    if (video_) video_->Stop();
    video_.reset();
    still_.clear();
    still_.shrink_to_fit();
    open_ = false;
    isStill_ = false;
    stillUploaded_ = false;
    path_.clear();
}

void CompareClip::FollowA(double posA, double startA, bool pausedA, VulkanRenderer* renderer, double nowSec) {
    if (!open_ || !renderer) return;
    if (isStill_) {
        if (!stillUploaded_ || !renderer->HasCompare())
            stillUploaded_ = renderer->UploadCompareTexture(still_.data(), (uint32_t)stillW_, (uint32_t)stillH_);
        return;
    }
    if (posA >= 0.0) {
        const SyncAction a = DecideSync(posA, startA, pausedA, video_->PositionSeconds(), video_->StartSeconds(),
                                        video_->FrameRate(), nowSec, lastSeekSec_);
        if (video_->Paused() != a.pause) video_->SetPaused(a.pause);
        const bool sameSeek = a.seekTo >= 0.0 && lastSeekTo_ >= 0.0 && std::fabs(a.seekTo - lastSeekTo_) < 1e-6 &&
                              nowSec - lastSeekSec_ < 1.0;
        if (a.seekTo >= 0.0 && !sameSeek) {
            video_->Seek(a.seekTo);
            lastSeekSec_ = nowSec;
            lastSeekTo_ = a.seekTo;
        }
    }
    if (const FrameRing::Frame* f = video_->Ring().AcquireLatest()) {
        if (f->gpu || f->plane[0].empty()) return;  // B decodes to CPU planes (no interop LUID)
        if (f->format == PixFormat::RGBA)
            renderer->UploadCompareTexture(f->plane[0].data(), (uint32_t)f->width, (uint32_t)f->height);
        else
            renderer->UploadCompareYUV(f->plane[0].data(), f->plane[1].data(),
                                       f->format == PixFormat::I420 ? f->plane[2].data() : nullptr,
                                       (uint32_t)f->width, (uint32_t)f->height,
                                       f->format == PixFormat::P016 ? 2 : f->format == PixFormat::NV12 ? 1 : 0,
                                       f->fullRange, (int)f->matrix);
    }
}

}  // namespace mp::pro
