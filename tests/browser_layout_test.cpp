// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for BrowserLayout: grid columns by width, rail or drawer, the view options'
// default state and the type chip's label, date sections (today, yesterday, months, time
// zones, the future), and a sectioned grid's tile and header positions, hit tests, visible
// ranges and scroll anchors.

#include "browser/BrowserLayout.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace mp::browser;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

constexpr int64_t kDay = 86400000;
// 2026-11-12 10:00 UTC.
constexpr int64_t kNow = 1794477600000;

void TestColumns() {
    std::printf("[about one column per 180 dp, at least two]\n");
    CHECK(GridColumns(360.0f) == 2, "a portrait phone: 2");
    CHECK(GridColumns(800.0f) == 4, "a landscape phone or a portrait tablet: 4");
    CHECK(GridColumns(1280.0f) == 7, "a landscape tablet: 7");
    CHECK(GridColumns(1280.0f - 280.0f) == 6, "beside a 280 dp rail: one fewer");
    CHECK(GridColumns(200.0f) == 2 && GridColumns(0.0f) == 2, "never fewer than 2");
}

void TestRail() {
    std::printf("[a rail on a wide landscape screen, else a drawer]\n");
    CHECK(UseRail(1280.0f, 800.0f), "landscape tablet: rail");
    CHECK(!UseRail(800.0f, 360.0f), "landscape phone: drawer");
    CHECK(!UseRail(800.0f, 1280.0f), "portrait tablet: drawer");
    CHECK(UseRail(840.0f, 600.0f), "840 dp is enough");
}

void TestSourceRow() {
    std::printf("[a source row: the label stops before the count]\n");
    SourceRowLayout r = SourceRow(100.0f, 800.0f, 60.0f, 40.0f);
    CHECK(Near(r.countX, 740.0f), "the count is right-aligned");
    CHECK(Near(r.labelMax, 600.0f), "the label runs to a gap before it");
    CHECK(100.0f + r.labelMax + 40.0f <= r.countX + 0.01f, "label + gap never reach the count");
    r = SourceRow(100.0f, 180.0f, 60.0f, 40.0f);
    CHECK(Near(r.labelMax, 0.0f), "no room: an empty label, never negative");
    CHECK(Near(SourceRow(10.0f, 200.0f, 0.0f, 16.0f).labelMax, 174.0f), "no count: the whole row");
}

void TestRailWidth() {
    std::printf("[the rail fits its rows within its limits]\n");
    CHECK(Near(RailWidth(500.0f, 2400.0f, 720.0f, 0.4f), 720.0f), "at least the minimum");
    CHECK(Near(RailWidth(800.0f, 2400.0f, 720.0f, 0.4f), 800.0f), "as wide as the rows need");
    CHECK(Near(RailWidth(1200.0f, 2400.0f, 720.0f, 0.4f), 960.0f), "at most 40% of the screen");
    CHECK(Near(RailWidth(1200.0f, 1000.0f, 720.0f, 0.4f), 720.0f), "the minimum wins over the fraction");
    CHECK(RailWidth(1200.0f, 500.0f, 720.0f, 0.4f) <= 500.0f, "never wider than the screen");
}

void TestOptions() {
    std::printf("[the overflow menu's options and the type chip]\n");
    ViewOptions o;
    CHECK(ViewOptionsAreDefault(o, false), "as they start");
    CHECK(!ViewOptionsAreDefault(o, true), "showing unsupported files");
    o.order = SortOrder::OldestFirst;
    CHECK(!ViewOptionsAreDefault(o, false), "oldest first");
    o = ViewOptions();
    o.type = TypeFilter::Videos;
    CHECK(ViewOptionsAreDefault(o, false), "the chips' filters are not counted");
    CHECK(std::strcmp(TypeLabel(TypeFilter::All), "All types") == 0, "label: all");
    CHECK(std::strcmp(TypeLabel(TypeFilter::Videos), "Videos") == 0, "label: videos");
    CHECK(std::strcmp(TypeLabel(TypeFilter::Images), "Photos") == 0, "label: photos");
}

void TestDates() {
    std::printf("[date sections]\n");
    CHECK(DateBucket(kNow, kNow, 0).label == "Today", "now: today");
    CHECK(DateBucket(kNow - 9 * 3600000LL, kNow, 0).label == "Today", "01:00 the same day: today");
    CHECK(DateBucket(kNow + kDay, kNow, 0).label == "Today", "the future: today");
    CHECK(DateBucket(kNow - kDay, kNow, 0).label == "Yesterday", "a day ago: yesterday");
    CHECK(DateBucket(kNow - 3 * kDay, kNow, 0).label == "November 2026", "earlier this month");
    CHECK(DateBucket(kNow - 20 * kDay, kNow, 0).label == "October 2026", "last month");
    CHECK(DateBucket(0, kNow, 0).label == "January 1970", "the epoch");
    CHECK(DateBucket(kNow - 320 * kDay, kNow, 0).label == "December 2025", "across a year");
    CHECK(DateBucket(1709164800000LL, kNow, 0).label == "February 2024", "2024-02-29, a leap day");
    // 23:30 UTC on the 11th is the 12th in UTC+2: today there, yesterday in UTC.
    const int64_t late = kNow - 10 * 3600000LL - 30 * 60000LL;
    CHECK(DateBucket(late, kNow, 0).label == "Yesterday", "UTC: yesterday");
    CHECK(DateBucket(late, kNow, 2 * 3600).label == "Today", "UTC+2: today");
    CHECK(DateBucket(kNow - 3 * kDay, kNow, 0).key == DateBucket(kNow - 5 * kDay, kNow, 0).key,
          "one month, one key");
    CHECK(DateBucket(kNow, kNow, 0).key != DateBucket(kNow - kDay, kNow, 0).key, "today and yesterday differ");
    CHECK(DateBucket(kNow - 20 * kDay, kNow, 0).key != DateBucket(kNow - 3 * kDay, kNow, 0).key,
          "months differ");
}

void TestGridPlain() {
    std::printf("[a grid with no headers]\n");
    GridLayout g;
    g.Build({7}, false, 3, 100.0f, 4.0f, 30.0f);
    CHECK(g.Tiles() == 7 && g.Headers() == 0, "7 tiles, no headers");
    CHECK(Near(g.Height(), 3 * 104.0f), "3 rows");
    float x = 0, y = 0;
    CHECK(g.TilePos(4, x, y) && Near(x, 104.0f) && Near(y, 104.0f), "tile 4: row 1, column 1");
    CHECK(!g.TilePos(7, x, y), "past the end");
    size_t p = 0;
    CHECK(g.TileAt(10.0f, 220.0f, p) && p == 6, "a point in tile 6");
    CHECK(!g.TileAt(102.0f, 10.0f, p), "a gutter");
    CHECK(!g.TileAt(120.0f, 220.0f, p), "past the last tile of the last row");
    size_t first = 0, end = 0;
    g.Range(110.0f, 150.0f, first, end);
    CHECK(first == 3 && end == 6, "a view inside row 1");
    g.Range(50.0f, 250.0f, first, end);
    CHECK(first == 0 && end == 7, "rows 0 to 2");
}

void TestGridSections() {
    std::printf("[a grid in sections]\n");
    GridLayout g;
    // Sections of 2, 0 (skipped) and 4 tiles, 3 columns, 100 px tiles, 4 px gutters, 30 px headers.
    g.Build({2, 0, 4}, true, 3, 100.0f, 4.0f, 30.0f);
    CHECK(g.Tiles() == 6 && g.Headers() == 2, "6 tiles under 2 headers");
    CHECK(Near(g.HeaderY(0), 0.0f) && Near(g.HeaderY(1), 134.0f), "headers on top of their sections");
    float x = 0, y = 0;
    CHECK(g.TilePos(1, x, y) && Near(x, 104.0f) && Near(y, 30.0f), "tile 1 under the first header");
    CHECK(g.TilePos(2, x, y) && Near(x, 0.0f) && Near(y, 164.0f), "a section starts a new row");
    CHECK(g.TilePos(5, x, y) && Near(x, 0.0f) && Near(y, 268.0f), "and wraps in it");
    CHECK(Near(g.Height(), 372.0f), "the height counts headers and rows");
    size_t p = 0;
    CHECK(!g.TileAt(10.0f, 10.0f, p), "a header is not a tile");
    CHECK(!g.TileAt(220.0f, 40.0f, p), "an empty cell of a short row");
    CHECK(g.TileAt(220.0f, 170.0f, p) && p == 4, "a tile in the second section");
    size_t first = 0, end = 0;
    g.Range(140.0f, 200.0f, first, end);
    CHECK(first == 2 && end == 5, "the second section's first row");
    CHECK(g.TileAtOffset(0.0f) == 0 && g.TileAtOffset(170.0f) == 2, "the tile at the top of a view");
    CHECK(Near(g.OffsetOf(2), 134.0f), "a section's first row comes back with its header");
    CHECK(Near(g.OffsetOf(5), 268.0f), "a later row on its own");
}

}  // namespace

int main() {
    TestColumns();
    TestRail();
    TestSourceRow();
    TestRailWidth();
    TestOptions();
    TestDates();
    TestGridPlain();
    TestGridSections();
    if (g_failures) {
        std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all browser layout checks passed\n");
    return 0;
}
