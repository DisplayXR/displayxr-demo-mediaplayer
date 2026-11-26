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
        clockB_.store(-1.0);
        video_->SetMasterClock([this] { return clockB_.load(); });
        if (!video_->Open(path)) {
            video_.reset();
            if (err) *err = "B does not open as a video";
            return false;
        }
        // No loop of its own (#250): at its end B holds its last frame until A's wrap
        // (or seek) sends it back, so the two wrap together.
        video_->SetLoop(false);
        isStill_ = false;
    }
    path_ = path;
    open_ = true;
    stillUploaded_ = false;
    lastSeekSec_ = -10.0;
    lastSeekTo_ = -1.0;
    holds_ = HoldStats{};
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
        const double startB = video_->StartSeconds();
        const double durB = video_->DurationSeconds();
        clockB_.store(startB + (posA - startA));
        const SyncAction a = DecideSync(posA, startA, pausedA, video_->PositionSeconds(), startB,
                                        video_->FrameRate(), nowSec, lastSeekSec_, durB > 0.0 ? startB + durB : -1.0);
        // At its end B pauses itself (loop off); un-pausing it there would only spin its EOF.
        if (video_->Paused() != a.pause && !(video_->Ended() && !a.pause)) video_->SetPaused(a.pause);
        if (a.pause) holds_.Break();
        const bool sameSeek = a.seekTo >= 0.0 && lastSeekTo_ >= 0.0 && std::fabs(a.seekTo - lastSeekTo_) < 1e-6 &&
                              nowSec - lastSeekSec_ < 1.0;
        if (a.seekTo >= 0.0 && !sameSeek) {
            LOG_DEBUG("Pro: compare B seek -> %.3f (A %.3f, B %.3f%s)", a.seekTo, posA, video_->PositionSeconds(),
                      video_->Ended() ? ", ended" : "");
            video_->Seek(a.seekTo);
            lastSeekSec_ = nowSec;
            lastSeekTo_ = a.seekTo;
            ++holds_.seeks;
            holds_.Break();
        }
    } else {
        clockB_.store(-1.0);
    }
    LogHolds(nowSec);
    if (const FrameRing::Frame* f = video_->Ring().AcquireLatest()) {
        if (!pausedA && !video_->Ended()) holds_.Frame(nowSec);
        if (f->gpu || f->plane[0].empty()) return;  // B decodes to CPU planes (no interop LUID)
        if (f->format == PixFormat::RGBA)
            renderer->UploadCompareTexture(f->plane[0].data(), (uint32_t)f->width, (uint32_t)f->height);
        else if (renderer->UploadCompareYUV(f->plane[0].data(), f->plane[1].data(),
                                            f->format == PixFormat::I420 ? f->plane[2].data() : nullptr,
                                            (uint32_t)f->width, (uint32_t)f->height,
                                            f->format == PixFormat::P016 ? 2 : f->format == PixFormat::NV12 ? 1 : 0,
                                            f->fullRange, (int)f->matrix)) {
            // B's HDR tags (#169), the way App tags A: HDR on the BT.2020 matrix is BT.2020.
            const bool hdr = f->transfer == ColorTransfer::HLG || f->transfer == ColorTransfer::PQ;
            renderer->SetCompareHdr((int)f->transfer, f->primaries2020 || (hdr && f->matrix == ColorMatrix::BT2020),
                                    f->peakNits);
        }
    }
}

// Every 30 s while B is open: how often B held a frame while A played.
void CompareClip::LogHolds(double nowSec) {
    if (holds_.periodStart < 0.0) holds_.periodStart = nowSec;
    if (nowSec - holds_.periodStart < 30.0) return;
    LOG_INFO("Pro: compare B: %d hold(s) over 250 ms in %.0f s (longest %.2f s), %d seek(s)", holds_.holds,
             nowSec - holds_.periodStart, holds_.longest, holds_.seeks);
    const double last = holds_.lastFrame;
    holds_ = HoldStats{};
    holds_.periodStart = nowSec;
    holds_.lastFrame = last;
}

}  // namespace mp::pro
