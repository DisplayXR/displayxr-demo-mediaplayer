// SPDX-License-Identifier: Apache-2.0
//
// CloudFetch: download a cloud-only file (a OneDrive placeholder) in the background, so it
// can be opened (#252). Opening a placeholder directly makes the first read wait for the
// whole download, on whatever thread reads: on the render thread that froze the player
// for as long as the download took, and for good when the download was cancelled.
//
// One file at a time. The download runs on its own thread through the Cloud Files API
// (CfHydratePlaceholder, asynchronous, so Cancel() stops it), with progress from the
// file's on-disk ranges; a file the API does not handle is read through instead. Windows
// only.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace mp::browser::win {

// Whether `path` (UTF-8) is cloud-only now: opening it would download it.
bool IsCloudOnlyPath(const std::string& path);

class CloudFetch {
public:
    enum class State { Idle, Running, Done, Failed, Cancelled };

    CloudFetch() = default;
    ~CloudFetch();
    CloudFetch(const CloudFetch&) = delete;
    CloudFetch& operator=(const CloudFetch&) = delete;

    // Start downloading `path`, cancelling a download already running. False if the file
    // cannot be opened.
    bool Start(const std::string& path);
    // Stop the download (the part already downloaded stays). State goes to Cancelled.
    void Cancel();
    // The state, the file and its progress (0..1, or < 0 when unknown).
    State Poll(std::string& path, float& progress) const;
    // Back to Idle after a finished, failed or cancelled download.
    void Clear();
    bool Busy() const { return state_.load() == State::Running; }
    // The file's size, bytes (0 until known).
    int64_t Size() const { return size_.load(); }

private:
    void Run(std::string path);
    void Join();

    std::thread thread_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> cancel_{false};
    std::atomic<float> progress_{-1.0f};
    std::atomic<int64_t> size_{0};
    mutable std::mutex mutex_;
    std::string path_;
    void* handle_ = nullptr;      // the file, while the download runs (under mutex_)
    void* overlapped_ = nullptr;  // its pending request (under mutex_)
};

} // namespace mp::browser::win
