// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for QcExporter: jobs run off the caller's thread, every job runs (none is
// superseded) in submit order, Busy() covers queued and running work, and the destructor
// finishes the queued jobs before it returns (a report asked for just before quitting is
// still written).

#include "pro/QcExporter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using mp::pro::QcExporter;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL (%s:%d): ", __FILE__, __LINE__);  \
            std::fprintf(stderr, __VA_ARGS__);                             \
            std::fprintf(stderr, "\n");                                    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Polls Take() until `n` results arrive or ~5 s pass.
std::vector<QcExporter::Result> TakeN(QcExporter& ex, size_t n) {
    std::vector<QcExporter::Result> out;
    for (int i = 0; i < 500 && out.size() < n; ++i) {
        QcExporter::Result r;
        while (out.size() < n && ex.Take(r)) out.push_back(r);
        if (out.size() < n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return out;
}

QcExporter::Job SlowJob(const std::string& dir, int ms, bool ok = true) {
    return [dir, ms, ok] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        QcExporter::Result r;
        r.ok = ok;
        r.dir = dir;
        if (!ok) r.err = "cannot write " + dir;
        return r;
    };
}

void TestOffThreadAndBusy() {
    QcExporter ex;
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> onOther{false};
    CHECK(!ex.Busy(), "idle exporter reports busy");
    const auto t0 = std::chrono::steady_clock::now();
    ex.Submit([&] {
        onOther = std::this_thread::get_id() != caller;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        QcExporter::Result r;
        r.ok = true;
        r.dir = "a";
        return r;
    });
    const double submitMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(submitMs < 50.0, "Submit() blocked the caller for %.1f ms", submitMs);
    CHECK(ex.Busy(), "a queued job does not show as busy");
    QcExporter::Result r;
    CHECK(!ex.Take(r), "a result before the job finished");
    const auto got = TakeN(ex, 1);
    CHECK(got.size() == 1, "no result after the job ran");
    CHECK(onOther, "the job ran on the caller's thread");
    if (!got.empty()) {
        CHECK(got[0].ok && got[0].dir == "a", "wrong result");
        CHECK(got[0].ms >= 100.0, "worker time not measured (%.1f ms)", got[0].ms);
    }
    CHECK(!ex.Busy(), "busy after the only job finished");
}

void TestEveryJobInOrder() {
    QcExporter ex;
    ex.Submit(SlowJob("first", 60));
    ex.Submit(SlowJob("second", 10, /*ok=*/false));
    ex.Submit(SlowJob("third", 10));
    const auto got = TakeN(ex, 3);
    CHECK(got.size() == 3, "%zu of 3 jobs reported (a job was superseded?)", got.size());
    if (got.size() == 3) {
        CHECK(got[0].dir == "first" && got[1].dir == "second" && got[2].dir == "third",
              "out of order: %s %s %s", got[0].dir.c_str(), got[1].dir.c_str(), got[2].dir.c_str());
        CHECK(got[0].ok && !got[1].ok && got[2].ok, "ok flags not carried");
        CHECK(got[1].err == "cannot write second", "error text not carried");
    }
}

void TestDestructorFinishesQueue() {
    auto ran = std::make_shared<std::atomic<int>>(0);
    {
        QcExporter ex;
        for (int i = 0; i < 3; ++i)
            ex.Submit([ran] {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                ++*ran;
                return QcExporter::Result{};
            });
    }  // quit right after asking
    CHECK(ran->load() == 3, "destructor dropped queued jobs: %d of 3 ran", ran->load());
}

void TestEmptyJob() {
    QcExporter ex;
    ex.Submit(QcExporter::Job{});  // an empty function reports a failure, never crashes
    const auto got = TakeN(ex, 1);
    CHECK(got.size() == 1 && !got[0].ok, "empty job not reported as a failure");
}

}  // namespace

int main() {
    TestOffThreadAndBusy();
    TestEveryJobInOrder();
    TestDestructorFinishesQueue();
    TestEmptyJob();
    if (g_failures) {
        std::fprintf(stderr, "qc_exporter_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("qc_exporter_test: all passed\n");
    return 0;
}
