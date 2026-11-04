// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the media browser's ThumbnailCache (#160): media id to atlas slot, empty
// slots first, then the least recently drawn one, and never a slot drawn this frame.

#include "browser/ThumbnailCache.h"

#include <cstdio>

using mp::browser::ThumbnailCache;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

void TestAssignAndUse() {
    ThumbnailCache cache;
    cache.Reset(2);
    int a = -1, b = -1, slot = -1;
    CHECK(cache.Assign(10, a) && cache.Assign(20, b) && a != b, "two ids take the two slots");
    CHECK(cache.Contains(10) && cache.Contains(20) && !cache.Contains(30), "contains what was assigned");
    CHECK(cache.Use(20, slot) && slot == b, "Use finds the slot");
    CHECK(!cache.Use(30, slot), "Use fails for an id with no slot");
    CHECK(cache.Assign(10, slot) && slot == a, "assigning an id again keeps its slot");
    CHECK(!cache.Assign(30, slot), "no slot when every slot was drawn this frame");
    CHECK(cache.Contains(10) && cache.Contains(20), "a failed assign takes nothing away");
}

void TestLeastRecentlyDrawn() {
    ThumbnailCache cache;
    cache.Reset(3);
    int s10 = -1, s20 = -1, s30 = -1, slot = -1;
    cache.Assign(10, s10);
    cache.Assign(20, s20);
    cache.Assign(30, s30);
    cache.NextFrame();
    cache.Use(10, slot);
    cache.NextFrame();
    cache.Use(30, slot);
    cache.NextFrame();
    CHECK(cache.Assign(40, slot) && slot == s20, "takes the slot drawn longest ago (20)");
    CHECK(!cache.Contains(20) && cache.Contains(40), "the old item loses it");
    CHECK(cache.Assign(50, slot) && slot == s10, "then the next oldest (10)");
    CHECK(cache.Assign(60, slot) && slot == s30, "then 30, the last one drawn in an earlier frame");
    CHECK(!cache.Assign(70, slot), "then none: all were drawn this frame");
}

void TestRemove() {
    ThumbnailCache cache;
    cache.Reset(2);
    int a = -1, b = -1, slot = -1;
    cache.Assign(10, a);
    cache.Assign(20, b);
    cache.Remove(10);
    CHECK(!cache.Contains(10), "removed");
    CHECK(cache.Assign(30, slot) && slot == a, "an emptied slot is taken, even in the same frame");
    CHECK(cache.Contains(20), "without taking another item's slot");

    cache.Reset(1);
    CHECK(!cache.Contains(20) && !cache.Contains(30), "Reset forgets everything");
    CHECK(cache.Assign(40, slot) && slot == 0, "and empties the slots");
}

}  // namespace

int main() {
    TestAssignAndUse();
    TestLeastRecentlyDrawn();
    TestRemove();
    if (g_failures) {
        std::fprintf(stderr, "thumbnail_cache_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("thumbnail_cache_test: all passed\n");
    return 0;
}
