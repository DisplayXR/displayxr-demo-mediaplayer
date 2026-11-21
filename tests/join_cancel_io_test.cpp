// SPDX-License-Identifier: Apache-2.0
//
// Unit test for JoinCancellingIo (#251): a worker blocked in a synchronous read that never
// completes (the stand-in for a file read stalled on a network share) is joined promptly,
// and its read fails with ERROR_OPERATION_ABORTED. Windows only; a pass elsewhere.

#include "platform/JoinCancelIo.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
#if defined(_WIN32)
    int failures = 0;
    // A pipe nobody writes to: ReadFile on its read end blocks until cancelled.
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, nullptr, 0)) {
        std::fprintf(stderr, "  FAIL: CreatePipe (%lu)\n", GetLastError());
        return 1;
    }
    for (int round = 0; round < 3; ++round) {
        std::atomic<bool> stop{false};
        std::atomic<bool> reading{false};
        DWORD readError = 0;
        int reads = 0;
        std::thread worker([&] {
            // Like a decode loop: read until stopped, retrying a failed read.
            while (!stop.load()) {
                char buf[16];
                DWORD got = 0;
                reading.store(true);
                ++reads;
                if (!ReadFile(rd, buf, sizeof(buf), &got, nullptr)) readError = GetLastError();
            }
        });
        while (!reading.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // into the blocking read
        const auto t0 = std::chrono::steady_clock::now();
        stop.store(true);
        mp::JoinCancellingIo(worker);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("round %d: joined in %.0f ms after %d read(s), error %lu\n", round, ms, reads, readError);
        if (ms > 2000.0) {
            std::fprintf(stderr, "  FAIL: join took %.0f ms\n", ms);
            ++failures;
        }
        if (readError != ERROR_OPERATION_ABORTED) {
            std::fprintf(stderr, "  FAIL: read error %lu, expected ERROR_OPERATION_ABORTED\n", readError);
            ++failures;
        }
        if (worker.joinable()) {
            std::fprintf(stderr, "  FAIL: still joinable\n");
            ++failures;
        }
    }
    // A thread that already finished, and an empty one, join as before.
    std::thread done([] {});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mp::JoinCancellingIo(done);
    std::thread empty;
    mp::JoinCancellingIo(empty);
    CloseHandle(rd);
    CloseHandle(wr);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
#else
    std::printf("Windows only: skipped\n");
    return 0;
#endif
}
