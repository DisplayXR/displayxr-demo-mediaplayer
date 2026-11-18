// SPDX-License-Identifier: Apache-2.0
#include "browser/BrowserLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mp::browser {

namespace {

constexpr float kColumnDp = 180.0f;
constexpr float kRailMinDp = 840.0f;
constexpr int64_t kDayKey = 1000000000;  // above every month key (year * 12 + month)

int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

// The civil date of day `days` since 1970-01-01 (H. Hinnant's days_from_civil, inverted).
void CivilFromDays(int64_t days, int& year, int& month) {
    days += 719468;
    const int64_t era = FloorDiv(days, 146097);
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    month = (int)(mp < 10 ? mp + 3 : mp - 9);
    year = (int)(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

}  // namespace

int GridColumns(float widthDp) {
    if (!(widthDp > 0.0f)) return 2;
    return std::max(2, (int)std::lround(widthDp / kColumnDp));
}

bool UseRail(float widthDp, float heightDp) { return widthDp >= heightDp && widthDp >= kRailMinDp; }

SourceRowLayout SourceRow(float labelX, float right, float countWidth, float gap) {
    SourceRowLayout r;
    r.countX = right - std::max(0.0f, countWidth);
    r.labelMax = std::max(0.0f, r.countX - gap - labelX);
    return r;
}

float RailWidth(float content, float screenWidth, float minWidth, float maxFraction) {
    const float most = std::max(0.0f, screenWidth * maxFraction);
    return std::min(std::max(content, minWidth), std::max(most, std::min(minWidth, screenWidth)));
}

bool ViewOptionsAreDefault(const ViewOptions& options, bool showUnsupported) {
    return options.order == ViewOptions().order && !showUnsupported;
}

const char* TypeLabel(TypeFilter type) {
    switch (type) {
        case TypeFilter::Videos: return "Videos";
        case TypeFilter::Images: return "Photos";
        default: return "All types";
    }
}

DateSection DateBucket(int64_t dateMs, int64_t nowMs, int64_t utcOffsetSeconds) {
    static const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                    "July",    "August",   "September", "October", "November", "December"};
    const int64_t day = FloorDiv(FloorDiv(dateMs, 1000) + utcOffsetSeconds, 86400);
    const int64_t today = FloorDiv(FloorDiv(nowMs, 1000) + utcOffsetSeconds, 86400);
    DateSection s;
    if (day >= today) {
        s.key = kDayKey + today;
        s.label = "Today";
    } else if (day == today - 1) {
        s.key = kDayKey + day;
        s.label = "Yesterday";
    } else {
        int year = 1970, month = 1;
        CivilFromDays(day, year, month);
        s.key = (int64_t)year * 12 + (month - 1);
        s.label = std::string(kMonths[month - 1]) + " " + std::to_string(year);
    }
    return s;
}

void GridLayout::Build(const std::vector<size_t>& sections, bool headers, int columns, float tile, float gutter,
                       float headerHeight, float tileHeight) {
    rows_.clear();
    headerY_.clear();
    columns_ = std::max(1, columns);
    tile_ = tile;
    step_ = tile + gutter;
    tileH_ = tileHeight < 0.0f ? tile : tileHeight;
    stepY_ = tileH_ + gutter;
    header_ = headers ? headerHeight : 0.0f;
    tiles_ = 0;
    float y = 0.0f;
    for (size_t count : sections) {
        if (count == 0) continue;
        if (headers) {
            headerY_.push_back(y);
            y += header_;
        }
        for (size_t done = 0; done < count; done += (size_t)columns_) {
            Row row;
            row.y = y;
            row.first = tiles_ + done;
            row.count = std::min((size_t)columns_, count - done);
            row.sectionStart = done == 0;
            rows_.push_back(row);
            y += stepY_;
        }
        tiles_ += count;
    }
    height_ = y;
}

int GridLayout::RowOf(size_t p) const {
    if (p >= tiles_ || rows_.empty()) return -1;
    // The last row whose first tile is at or before p.
    auto it = std::upper_bound(rows_.begin(), rows_.end(), p, [](size_t v, const Row& r) { return v < r.first; });
    return (int)(it - rows_.begin()) - 1;
}

bool GridLayout::TilePos(size_t p, float& x, float& y) const {
    const int r = RowOf(p);
    if (r < 0) return false;
    x = (float)(p - rows_[(size_t)r].first) * step_;
    y = rows_[(size_t)r].y;
    return true;
}

bool GridLayout::TileAt(float x, float y, size_t& p) const {
    if (x < 0.0f || y < 0.0f || rows_.empty()) return false;
    auto it = std::upper_bound(rows_.begin(), rows_.end(), y, [](float v, const Row& r) { return v < r.y; });
    if (it == rows_.begin()) return false;
    const Row& row = *(it - 1);
    if (y - row.y > tileH_) return false;  // the gutter below it, or a header
    const int c = (int)(x / step_);
    if (c >= columns_ || x - (float)c * step_ > tile_ || (size_t)c >= row.count) return false;
    p = row.first + (size_t)c;
    return true;
}

void GridLayout::Range(float y0, float y1, size_t& first, size_t& end) const {
    first = end = 0;
    if (rows_.empty() || y1 <= y0) return;
    // The first row ending below y0, and the first row starting at or below y1.
    auto lo = std::upper_bound(rows_.begin(), rows_.end(), y0 - stepY_, [](float v, const Row& r) { return v < r.y; });
    auto hi = std::lower_bound(rows_.begin(), rows_.end(), y1, [](const Row& r, float v) { return r.y < v; });
    if (lo >= hi) return;
    first = lo->first;
    const Row& last = *(hi - 1);
    end = last.first + last.count;
}

size_t GridLayout::TileAtOffset(float offset) const {
    if (rows_.empty()) return 0;
    auto it = std::upper_bound(rows_.begin(), rows_.end(), offset + stepY_ * 0.5f,
                               [](float v, const Row& r) { return v < r.y; });
    if (it == rows_.begin()) return 0;
    return (it - 1)->first;
}

void ClickSelect(const std::vector<int64_t>& ids, size_t at, bool ctrl, bool shift, int64_t& anchor,
                 std::unordered_set<int64_t>& selected) {
    if (at >= ids.size()) return;
    if (shift) {
        size_t from = at;
        const auto it = std::find(ids.begin(), ids.end(), anchor);
        if (it != ids.end()) from = (size_t)(it - ids.begin());
        else anchor = ids[at];
        if (!ctrl) selected.clear();
        for (size_t i = std::min(from, at); i <= std::max(from, at); ++i) selected.insert(ids[i]);
        return;
    }
    anchor = ids[at];
    if (ctrl) {
        if (!selected.erase(ids[at])) selected.insert(ids[at]);
        return;
    }
    selected.clear();
    selected.insert(ids[at]);
}

std::string FormatDeskViewPrefs(const DeskViewPrefs& prefs) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "view=%s\ncard=%d\nsort=%s\n", prefs.details ? "details" : "grid",
                  (int)std::lround(prefs.cardW), prefs.order == SortOrder::OldestFirst ? "oldest" : "newest");
    return buf;
}

DeskViewPrefs ParseDeskViewPrefs(const std::string& text, float cardMin, float cardMax) {
    DeskViewPrefs p;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "view") {
            if (value == "details") p.details = true;
            else if (value == "grid") p.details = false;
        } else if (key == "card") {
            char* stop = nullptr;
            const double v = std::strtod(value.c_str(), &stop);
            if (stop != value.c_str() && *stop == '\0' && std::isfinite(v))
                p.cardW = std::min(cardMax, std::max(cardMin, (float)v));
        } else if (key == "sort") {
            if (value == "oldest") p.order = SortOrder::OldestFirst;
            else if (value == "newest") p.order = SortOrder::NewestFirst;
        }
    }
    return p;
}

float GridLayout::OffsetOf(size_t p) const {
    const int r = RowOf(p);
    if (r < 0) return 0.0f;
    const Row& row = rows_[(size_t)r];
    return row.sectionStart ? row.y - header_ : row.y;
}

}  // namespace mp::browser
