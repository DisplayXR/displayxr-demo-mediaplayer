// SPDX-License-Identifier: Apache-2.0
#include "pro/QcExporter.h"

#include <chrono>
#include <exception>
#include <string>
#include <system_error>
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
        try {
            if (job) r = job();
        } catch (const std::exception& e) {  // e.g. no memory for an 8K snapshot's thumbnail
            r = Result{};
            r.err = e.what();
        }
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
            done_.push_back(std::move(r));
        }
    }
}

namespace qcfiles {

bool CreateUniqueDir(const std::filesystem::path& parent, const std::string& base, std::filesystem::path& out,
                     std::error_code& ec) {
    ec.clear();
    std::filesystem::create_directories(parent, ec);
    if (ec) return false;
    for (int k = 1; k <= 99; ++k) {
        const std::filesystem::path d = parent / (k == 1 ? base : base + "-" + std::to_string(k));
        ec.clear();
        if (std::filesystem::create_directory(d, ec)) {  // false + no error: it exists, try the next
            out = d;
            return true;
        }
        // A FILE of that name: libstdc++ says false/no error, MSVC reports an error. Either
        // way the name is taken; anything else (no permission, full disk) is a failure.
        std::error_code ec2;
        if (ec && !std::filesystem::exists(d, ec2)) return false;
    }
    ec = std::make_error_code(std::errc::file_exists);
    return false;
}

std::string UniqueName(const std::string& base, const std::function<bool(const std::string&)>& taken) {
    for (int k = 1; k <= 99; ++k) {
        const std::string n = k == 1 ? base : base + "-" + std::to_string(k);
        if (!taken(n)) return n;
    }
    return {};
}

bool PrefixInUse(const std::string& prefix) {
    if (prefix.empty()) return false;
    const std::filesystem::path pre = std::filesystem::u8path(prefix);
    const std::string stem = pre.filename().u8string() + "_";
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(pre.parent_path(), ec))
        if (de.path().filename().u8string().rfind(stem, 0) == 0) return true;
    return false;
}

std::string FindAtlasCapture(const std::string& prefix) {
    if (prefix.empty()) return {};
    const std::filesystem::path pre = std::filesystem::u8path(prefix);
    const std::string stem = pre.filename().u8string() + "_atlas";
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(pre.parent_path(), ec)) {
        const std::string f = de.path().filename().u8string();
        if (f.rfind(stem, 0) == 0 && de.path().extension() == ".png") return de.path().u8string();
    }
    return {};
}

}  // namespace qcfiles

}  // namespace mp::pro
