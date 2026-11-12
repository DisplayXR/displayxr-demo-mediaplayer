// SPDX-License-Identifier: Apache-2.0
#include "browser/StereoPairs.h"

#include "media/LifLoader.h"
#include "media/MpoLoader.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#if defined(__ANDROID__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace mp::browser {

namespace {

constexpr uint64_t kMaxViewBytes = 64u << 20;  // a view's JPEG larger than this is not read

} // namespace

void PairEyeSize(uint32_t w, uint32_t h, float scale, uint32_t& ew, uint32_t& eh) {
    if (!(scale > 0.0f) || scale > 1.0f) scale = 1.0f;
    ew = std::min(w, std::max(16u, (uint32_t)std::lround((double)w * scale)));
    eh = std::min(h, std::max(16u, (uint32_t)std::lround((double)h * scale)));
}

void ResizeRgba(const uint8_t* src, uint32_t w, uint32_t h, uint32_t ew, uint32_t eh, std::vector<uint8_t>& out) {
    out.assign((size_t)ew * eh * 4, 0);
    if (!src || w == 0 || h == 0 || ew == 0 || eh == 0) return;
    for (uint32_t y = 0; y < eh; ++y) {
        const uint32_t y0 = (uint32_t)((uint64_t)y * h / eh);
        const uint32_t y1 = std::max(y0 + 1, (uint32_t)((uint64_t)(y + 1) * h / eh));
        for (uint32_t x = 0; x < ew; ++x) {
            const uint32_t x0 = (uint32_t)((uint64_t)x * w / ew);
            const uint32_t x1 = std::max(x0 + 1, (uint32_t)((uint64_t)(x + 1) * w / ew));
            uint32_t sum[4] = {0, 0, 0, 0};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint8_t* p = src + ((size_t)sy * w + x0) * 4;
                for (uint32_t sx = x0; sx < x1; ++sx, p += 4)
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            uint8_t* d = &out[((size_t)y * ew + x) * 4];
            for (int c = 0; c < 4; ++c) d[c] = (uint8_t)((sum[c] + n / 2) / n);
        }
    }
}

void ComposePair(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right, uint32_t ew, uint32_t eh,
                 std::vector<uint8_t>& out) {
    const size_t row = (size_t)ew * 4;
    out.assign(row * 2 * eh, 0);
    if (left.size() < row * eh || right.size() < row * eh) return;
    for (uint32_t y = 0; y < eh; ++y) {
        std::copy(left.begin() + (long)(row * y), left.begin() + (long)(row * (y + 1)), out.begin() + (long)(row * 2 * y));
        std::copy(right.begin() + (long)(row * y), right.begin() + (long)(row * (y + 1)),
                  out.begin() + (long)(row * (2 * y + 1)));
    }
}

StereoPairs::~StereoPairs() { Shutdown(); }

bool StereoPairs::Initialize(ScaledJpegDecoder decode, FileReader read, std::function<void(int)> close) {
    if (worker_.joinable() || !decode || !read || !close) return false;
    decode_ = std::move(decode);
    read_ = std::move(read);
    close_ = std::move(close);
    stop_ = false;
    worker_ = std::thread([this] { Work(); });
    return true;
}

void StereoPairs::Shutdown() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
    for (Job& j : jobs_) close_(j.fd);
    jobs_.clear();
    done_.clear();
    busy_.clear();
    cache_.clear();
    lru_.clear();
    cacheBytes_ = 0;
}

void StereoPairs::Make(int64_t id, int64_t modified, int fd, uint64_t fileSize, const uint8_t* left, uint32_t w,
                       uint32_t h, float scale) {
    if (!worker_.joinable() || fd < 0 || !left || w == 0 || h == 0 || busy_.count(id)) {
        if (fd >= 0 && close_) close_(fd);
        return;
    }
    Job job;
    job.id = id;
    job.modified = modified;
    job.fd = fd;
    job.size = fileSize;
    job.w = w;
    job.h = h;
    job.scale = scale;
    job.left.assign(left, left + (size_t)w * h * 4);
    busy_.insert(id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        while (jobs_.size() > kMaxQueued) {  // scrolled past: the oldest are no longer wanted
            close_(jobs_.front().fd);
            Done d;
            d.id = jobs_.front().id;
            done_.push_back(std::move(d));
            jobs_.pop_front();
        }
    }
    wake_.notify_one();
}

void StereoPairs::TakeResults(std::vector<Thumbnail>& pairs, std::vector<int64_t>& failed) {
    std::deque<Done> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done.swap(done_);
    }
    for (Done& d : done) {
        busy_.erase(d.id);
        if (!d.ok) {
            failed.push_back(d.id);
            continue;
        }
        Keep(d.id, d.modified, d.sbs);
        pairs.push_back(std::move(d.sbs));
    }
}

void StereoPairs::Keep(int64_t id, int64_t modified, const Thumbnail& sbs) {
    auto it = cache_.find(id);
    if (it != cache_.end()) {
        cacheBytes_ -= it->second.sbs.rgba.size();
        lru_.erase(it->second.lru);
        cache_.erase(it);
    }
    if (sbs.rgba.size() > kCacheBytes) return;
    lru_.push_front(id);
    cache_[id] = Entry{modified, sbs, lru_.begin()};
    cacheBytes_ += sbs.rgba.size();
    while (cacheBytes_ > kCacheBytes && !lru_.empty()) {
        const int64_t old = lru_.back();
        lru_.pop_back();
        auto o = cache_.find(old);
        if (o != cache_.end()) {
            cacheBytes_ -= o->second.sbs.rgba.size();
            cache_.erase(o);
        }
    }
}

bool StereoPairs::Has(int64_t id, int64_t modified) const {
    const auto it = cache_.find(id);
    return it != cache_.end() && it->second.modified == modified;
}

bool StereoPairs::Cached(int64_t id, int64_t modified, Thumbnail& out) {
    auto it = cache_.find(id);
    if (it == cache_.end() || it->second.modified != modified) return false;
    lru_.erase(it->second.lru);
    lru_.push_front(id);
    it->second.lru = lru_.begin();
    out = it->second.sbs;
    return true;
}

void StereoPairs::Work() {
#if defined(__ANDROID__)
    setpriority(PRIO_PROCESS, (id_t)gettid(), 19);  // the lowest: behind opening and the stills
#endif
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Done done;
        done.id = job.id;
        done.modified = job.modified;
        const auto t0 = std::chrono::steady_clock::now();
        done.ok = MakeOne(job, done.sbs);
        close_(job.fd);
        statMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        statOk_ += done.ok ? 1 : 0;
        if (++statCount_ == 4) {
            LOG_INFO("3D grid: 4 photo pairs, %d made, %.0f ms each on average (eye %ux%u)", statOk_,
                     statMs_ / 4.0, done.sbs.width / 2, done.sbs.height);
            statCount_ = statOk_ = 0;
            statMs_ = 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(done));
    }
}

bool StereoPairs::MakeOne(const Job& job, Thumbnail& out) {
    const int fd = job.fd;
    auto readAt = [&](uint64_t offset, void* dst, size_t size) {
        return offset + size <= job.size && read_(fd, offset, dst, size);
    };
    LifLoader::ViewSpan left, right;
    bool found = LifLoader::FindViews(readAt, job.size, left, right);
    if (!found) {
        std::vector<uint8_t> head((size_t)std::min<uint64_t>(job.size, MpoLoader::SniffBytes()));
        uint64_t offset = 0, size = 0;
        if (!head.empty() && readAt(0, head.data(), head.size()) &&
            MpoLoader::FindSecondView(head, job.size, offset, size)) {
            left = LifLoader::ViewSpan{0, offset, true};
            right = LifLoader::ViewSpan{offset, size, false};
            found = true;
        }
    }
    if (!found || right.size == 0 || right.size > kMaxViewBytes || left.size > kMaxViewBytes) return false;

    uint32_t ew = 0, eh = 0;
    PairEyeSize(job.w, job.h, job.scale, ew, eh);
    auto decodeSpan = [&](const LifLoader::ViewSpan& s, std::vector<uint8_t>& rgba) {
        std::vector<uint8_t> bytes((size_t)s.size);
        return readAt(s.offset, bytes.data(), bytes.size()) && decode_(bytes.data(), bytes.size(), ew, eh, rgba) &&
               rgba.size() >= (size_t)ew * eh * 4;
    };
    std::vector<uint8_t> l, r;
    if (!decodeSpan(right, r)) return false;
    if (left.base) {
        ResizeRgba(job.left.data(), job.w, job.h, ew, eh, l);  // the system's thumbnail is this view
    } else if (!decodeSpan(left, l)) {
        return false;
    }
    out = Thumbnail{};
    out.id = job.id;
    out.width = ew * 2;
    out.height = eh;
    ComposePair(l, r, ew, eh, out.rgba);
    return true;
}

} // namespace mp::browser
