// SPDX-License-Identifier: Apache-2.0
#include "browser/ThumbnailDepth.h"

#include "media/AutoConvergence.h"

#include <utility>

namespace mp::browser {

ThumbnailDepth::~ThumbnailDepth() { Shutdown(); }

bool ThumbnailDepth::Initialize() {
    if (worker_.joinable()) return false;
    stop_ = false;
    worker_ = std::thread([this] { Work(); });
    return true;
}

void ThumbnailDepth::Shutdown() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    worker_.join();
    done_.clear();
    estimates_.clear();
    asked_.clear();
    order_.clear();
}

void ThumbnailDepth::Measure(int64_t id, const uint8_t* rgba, uint32_t w, uint32_t h, StereoLayout layout,
                             float cropFrac) {
    if (!worker_.joinable() || !rgba || w == 0 || h == 0 || !Packed(layout) || asked_.count(id)) return;
    asked_[id] = true;
    order_.push_back(id);
    while (order_.size() > kMaxKept) {
        asked_.erase(order_.front());
        estimates_.erase(order_.front());
        order_.pop_front();
    }
    Job job;
    job.id = id;
    job.w = w;
    job.h = h;
    job.layout = layout;
    job.cropFrac = cropFrac;
    job.rgba.assign(rgba, rgba + (size_t)w * h * 4);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void ThumbnailDepth::TakeResults() {
    std::deque<Done> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done.swap(done_);
    }
    for (const Done& d : done) {
        if (asked_.count(d.id)) estimates_[d.id] = d.estimate;  // not forgotten meanwhile
    }
}

bool ThumbnailDepth::Get(int64_t id, DepthEstimate& out) const {
    const auto it = estimates_.find(id);
    if (it == estimates_.end()) return false;
    out = it->second;
    return true;
}

void ThumbnailDepth::Forget(int64_t id) {
    asked_.erase(id);
    estimates_.erase(id);
    // order_ keeps the id; erasing a missing id later is harmless.
}

void ThumbnailDepth::Work() {
    std::vector<uint8_t> luma;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        int lw = 0, lh = 0;
        autoconv::StillLuma(job.rgba.data(), (int)job.w, (int)job.h, luma, lw, lh);
        Done done;
        done.id = job.id;
        done.estimate.cropFrac = job.cropFrac;
        if (!luma.empty()) {
            // A fresh estimator per thumbnail: its tracker and median hold belong to a feed.
            autoconv::DisparityEstimator estimator;
            const autoconv::Result r = estimator.Measure(luma.data(), lw, lw, lh, job.layout,
                                                         autoconv::SubjectPolicy::Nearest, nullptr, 0.0);
            done.estimate.ok = r.ok;
            done.estimate.subject = r.rawFrac;
            done.estimate.nearest = r.hasRange ? r.nearFrac : r.rawFrac;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(done);
    }
}

} // namespace mp::browser
