// SPDX-License-Identifier: Apache-2.0
#include "pro/LookLoader.h"

#include <chrono>
#include <utility>

namespace mp::pro {

LookLoader::LookLoader() : worker_([this] { Run(); }) {}

LookLoader::~LookLoader() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

uint64_t LookLoader::Submit(Work work, std::string source, bool isOcio) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        id = ++nextId_;
        latestId_ = id;
        job_ = std::move(work);  // a queued-but-unstarted request is simply replaced
        jobSource_ = std::move(source);
        jobIsOcio_ = isOcio;
        jobId_ = id;
        haveJob_ = true;
        haveResult_ = false;  // an older finished result is superseded too
    }
    cv_.notify_one();
    return id;
}

bool LookLoader::Take(Result& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!haveResult_) return false;
    out = std::move(result_);
    result_ = Result{};
    haveResult_ = false;
    return true;
}

bool LookLoader::Busy() const {
    std::lock_guard<std::mutex> lk(mu_);
    return haveJob_ || runningId_ != 0;
}

void LookLoader::Run() {
    for (;;) {
        Work work;
        Result r;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || haveJob_; });
            if (stop_) return;
            work = std::move(job_);
            r.id = jobId_;
            r.source = std::move(jobSource_);
            r.isOcio = jobIsOcio_;
            haveJob_ = false;
            runningId_ = r.id;
        }
        const auto t0 = std::chrono::steady_clock::now();
        r.ok = work && work(r.lut, r.err);
        if (r.ok && !r.lut.Valid()) {
            r.ok = false;
            r.err = "the look produced no LUT";
        }
        if (r.ok) r.rgba16f = color::PackRgba16f(r.lut.rgb.data(), r.lut.rgb.size() / 3);
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(mu_);
            runningId_ = 0;
            if (r.id == latestId_) {  // a newer Submit() makes this one stale: drop it
                result_ = std::move(r);
                haveResult_ = true;
            }
        }
    }
}

}  // namespace mp::pro
