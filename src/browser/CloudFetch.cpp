// SPDX-License-Identifier: Apache-2.0
#include "browser/CloudFetch.h"

#include "Log.h"
#include "browser/WinShellMedia.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cfapi.h>

#include <algorithm>
#include <vector>

namespace mp::browser::win {

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Bytes of the file already on disk, from its placeholder ranges; -1 if it cannot say.
int64_t OnDisk(HANDLE h, int64_t size) {
    std::vector<CF_FILE_RANGE> ranges(64);
    DWORD got = 0;
    LARGE_INTEGER start{}, length{};
    length.QuadPart = size;
    const HRESULT hr = CfGetPlaceholderRangeInfo(h, CF_PLACEHOLDER_RANGE_INFO_ONDISK, start, length, ranges.data(),
                                                 (DWORD)(ranges.size() * sizeof(CF_FILE_RANGE)), &got);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA)) return -1;
    int64_t sum = 0;
    for (size_t i = 0; i < got / sizeof(CF_FILE_RANGE); ++i) sum += ranges[i].Length.QuadPart;
    return sum;
}

} // namespace

bool IsCloudOnlyPath(const std::string& path) {
    const DWORD a = GetFileAttributesW(Wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && IsCloudOnly((uint32_t)a);
}

CloudFetch::~CloudFetch() {
    Cancel();
    Join();
}

bool CloudFetch::Start(const std::string& path) {
    Cancel();
    Join();
    // Opening the handle does not download anything; reading (or hydrating) does.
    HANDLE h = CreateFileW(Wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_WARN("cloud: cannot open '%s' (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        handle_ = h;
    }
    cancel_.store(false);
    progress_.store(-1.0f);
    state_.store(State::Running);
    thread_ = std::thread([this, path] { Run(path); });
    return true;
}

void CloudFetch::Cancel() {
    if (state_.load() != State::Running) return;
    cancel_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_) CancelIoEx((HANDLE)handle_, (LPOVERLAPPED)overlapped_);
}

CloudFetch::State CloudFetch::Poll(std::string& path, float& progress) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = path_;
    }
    progress = progress_.load();
    return state_.load();
}

void CloudFetch::Clear() {
    if (state_.load() == State::Running) return;
    Join();
    state_.store(State::Idle);
}

void CloudFetch::Join() {
    if (thread_.joinable()) thread_.join();
}

void CloudFetch::Run(std::string path) {
    HANDLE h;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        h = (HANDLE)handle_;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    const double t0 = NowMs();
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto setPending = [&](OVERLAPPED* o) {
        std::lock_guard<std::mutex> lock(mutex_);
        overlapped_ = o;
    };
    // Wait for the pending request, keeping the progress up to date, until it completes
    // (true), fails or is cancelled (false).
    auto wait = [&](DWORD& bytes) {
        for (;;) {
            const DWORD w = WaitForSingleObject(ov.hEvent, 200);
            if (w == WAIT_OBJECT_0) break;
            const int64_t on = OnDisk(h, size.QuadPart);
            if (on >= 0 && size.QuadPart > 0) progress_.store((float)((double)on / (double)size.QuadPart));
            if (cancel_.load()) CancelIoEx(h, &ov);
        }
        return GetOverlappedResult(h, &ov, &bytes, TRUE) != FALSE;
    };

    bool ok = false;
    setPending(&ov);
    LARGE_INTEGER start{}, all{};
    all.QuadPart = -1;  // CF_EOF: to the end of the file
    HRESULT hr = CfHydratePlaceholder(h, start, all, CF_HYDRATE_FLAG_NONE, &ov);
    if (hr == HRESULT_FROM_WIN32(ERROR_IO_PENDING)) {
        DWORD bytes = 0;
        ok = wait(bytes);
        hr = ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    } else {
        ok = SUCCEEDED(hr);
    }
    if (!ok && !cancel_.load()) {
        // Not a Cloud Files placeholder (another sync client): read it through instead.
        LOG_INFO("cloud: hydrate failed (0x%08lx), reading '%s' through", (unsigned long)hr, path.c_str());
        std::vector<char> buf(8u << 20);
        int64_t at = 0;
        ok = true;
        while (at < size.QuadPart && !cancel_.load()) {
            ResetEvent(ov.hEvent);
            ov.Offset = (DWORD)(at & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)(at >> 32);
            DWORD bytes = 0;
            if (!ReadFile(h, buf.data(), (DWORD)buf.size(), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
                ok = false;
                break;
            }
            if (!wait(bytes) || bytes == 0) {
                ok = false;
                break;
            }
            at += bytes;
            progress_.store((float)((double)at / (double)std::max<int64_t>(1, size.QuadPart)));
        }
        ok = ok && !cancel_.load();
    }
    setPending(nullptr);
    CloseHandle(ov.hEvent);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseHandle(h);
        handle_ = nullptr;
    }
    const double secs = (NowMs() - t0) / 1000.0;
    if (cancel_.load()) {
        LOG_INFO("cloud: download of '%s' cancelled after %.1f s", path.c_str(), secs);
        state_.store(State::Cancelled);
    } else if (ok) {
        LOG_INFO("cloud: downloaded '%s' (%.0f MB) in %.1f s", path.c_str(), (double)size.QuadPart / 1e6, secs);
        progress_.store(1.0f);
        state_.store(State::Done);
    } else {
        LOG_WARN("cloud: download of '%s' failed after %.1f s", path.c_str(), secs);
        state_.store(State::Failed);
    }
}

} // namespace mp::browser::win
