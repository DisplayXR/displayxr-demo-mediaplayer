// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for LookLoader: the work runs off the caller's thread, results carry the
// packed RGBA16F texels, errors come back as results, and the latest request wins.

#include "pro/LookLoader.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using mp::color::CubeLut;
using mp::pro::LookLoader;

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

// A 2^3 LUT whose every entry is `v`.
bool Flat(CubeLut& lut, float v) {
    lut.size = 2;
    lut.title = "flat";
    lut.rgb.assign(2 * 2 * 2 * 3, v);
    return true;
}

bool WaitTake(LookLoader& l, LookLoader::Result& r, int ms = 2000) {
    for (int i = 0; i < ms; ++i) {
        if (l.Take(r)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void RunsOffThreadAndPacks() {
    LookLoader l;
    std::atomic<bool> other{false};
    const auto caller = std::this_thread::get_id();
    l.Submit([&](CubeLut& lut, std::string&) {
        other = std::this_thread::get_id() != caller;
        return Flat(lut, 0.5f);
    }, "a.cube", false);
    LookLoader::Result r;
    CHECK(WaitTake(l, r), "a result arrives");
    CHECK(other, "the work ran on the loader's thread");
    CHECK(r.ok && r.source == "a.cube" && !r.isOcio, "ok, with its source");
    CHECK(r.rgba16f.size() == 8 * 4, "8 texels x RGBA");
    CHECK(r.rgba16f[0] == 0x3800u && r.rgba16f[3] == 0x3c00u, "0.5 -> 0x3800, alpha 1.0 -> 0x3c00");
    CHECK(!l.Take(r), "taken once");
    CHECK(!l.Busy(), "idle afterwards");
}

void ErrorsComeBackAsResults() {
    LookLoader l;
    l.Submit([](CubeLut&, std::string& err) { err = "bad file"; return false; }, "b.cube", false);
    LookLoader::Result r;
    CHECK(WaitTake(l, r), "a failed result arrives");
    CHECK(!r.ok && r.err == "bad file" && r.rgba16f.empty(), "error carried, nothing packed");
    // A work function that claims success without a LUT is a failure, not an empty upload.
    l.Submit([](CubeLut&, std::string&) { return true; }, "c.cube", true);
    CHECK(WaitTake(l, r), "the second result arrives");
    CHECK(!r.ok && r.isOcio, "an empty LUT is rejected");
}

void LatestWins() {
    LookLoader l;
    std::atomic<bool> release{false};
    // The first request blocks until released; two more are submitted meanwhile.
    l.Submit([&](CubeLut& lut, std::string&) {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return Flat(lut, 0.25f);
    }, "first", false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the worker start it
    l.Submit([](CubeLut& lut, std::string&) { return Flat(lut, 0.5f); }, "second", false);
    l.Submit([](CubeLut& lut, std::string&) { return Flat(lut, 1.0f); }, "third", false);
    CHECK(l.Busy(), "busy while the first is running");
    release = true;
    LookLoader::Result r;
    CHECK(WaitTake(l, r), "a result arrives");
    CHECK(r.source == "third", "only the newest request's result is delivered (got %s)", r.source.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!l.Take(r), "the superseded ones never arrive");
}

void DestroysWhileBusy() {
    std::atomic<bool> started{false};
    {
        LookLoader l;
        l.Submit([&](CubeLut& lut, std::string&) {
            started = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return Flat(lut, 0.5f);
        }, "slow", false);
        while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }  // the destructor joins the worker
    CHECK(started, "destroyed cleanly while a request ran");
}

}  // namespace

int main() {
    RunsOffThreadAndPacks();
    ErrorsComeBackAsResults();
    LatestWins();
    DestroysWhileBusy();
    if (g_failures != 0) {
        std::fprintf(stderr, "look_loader_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("look_loader_test: all passed\n");
    return 0;
}
