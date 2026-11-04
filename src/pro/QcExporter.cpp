// SPDX-License-Identifier: Apache-2.0
#include "pro/QcExporter.h"

#include <chrono>
#include <utility>

namespace mp::pro {

QcExporter::QcExporter() : worker_([this] { Run(); }) {}

QcExporter::~QcExporter() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void QcExporter::Submit(Job job) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

bool QcExporter::Take(Result& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (done_.empty()) return false;
    out = std::move(done_.front());
    done_.pop_front();
    return true;
}

bool QcExporter::Busy() const {
    std::lock_guard<std::mutex> lk(mu_);
    return running_ || !jobs_.empty();
}

void QcExporter::Run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return;  // stop_ and nothing left to write
            job = std::move(jobs_.front());
            jobs_.pop_front();
            running_ = true;
        }
        const auto t0 = std::chrono::steady_clock::now();
        Result r;
        if (job) r = job();
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
            done_.push_back(std::move(r));
        }
    }
}

}  // namespace mp::pro
