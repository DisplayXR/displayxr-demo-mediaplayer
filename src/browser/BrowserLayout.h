// SPDX-License-Identifier: Apache-2.0
//
// BrowserLayout: the browser's layout decisions as pure logic (#180), so they have unit
// tests: how many grid columns a width takes, rail or drawer, the date sections, whether
// the view options are the defaults, and where every tile and section header of a
// sectioned grid sits. Render thread; sizes in dp or pixels as named.
#pragma once

#include "browser/MediaLibrary.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace mp::browser {

enum class DepthFilter { Both, Only3D, Only2D };

// Grid columns for a grid `widthDp` wide: about one per 180 dp, never fewer than 2.
int GridColumns(float widthDp);

// The sources sit in a persistent rail beside the grid on a landscape screen 840 dp wide or
// more; anywhere else they are a drawer over it.
bool UseRail(float widthDp, float heightDp);

// A source row in the rail or the drawer: the label starts at `labelX`, the count is
// right-aligned to `right`, and the label may run only to `gap` before the count (it is cut
// with an ellipsis there), so the two never overlap. `labelMax` is never negative.
struct SourceRowLayout {
    float countX = 0.0f;
    float labelMax = 0.0f;
};
SourceRowLayout SourceRow(float labelX, float right, float countWidth, float gap);

// The rail's width: what its widest row needs (`content`), at least `minWidth`, at most
// `maxFraction` of the screen.
float RailWidth(float content, float screenWidth, float minWidth, float maxFraction);

// The view options behind the overflow menu (sort, show unsupported) are as they start.
// The filters in the chip row show their own state, so they are not counted.
bool ViewOptionsAreDefault(const ViewOptions& options, bool showUnsupported);

// The type chip's label.
const char* TypeLabel(TypeFilter type);

// Desktop (#252): how the browser looks, kept between launches: grid or details, the cards'
// width (design px), and the sort. As text, one "key=value" a line ("view=details",
// "card=200", "sort=oldest"); a line it does not know is skipped and a bad value keeps the
// default, so an older or a newer file still reads.
struct DeskViewPrefs {
    bool details = false;
    float cardW = 176.0f;
    SortOrder order = SortOrder::NewestFirst;
    bool operator==(const DeskViewPrefs& o) const {
        return details == o.details && cardW == o.cardW && order == o.order;
    }
    bool operator!=(const DeskViewPrefs& o) const { return !(*this == o); }
};
std::string FormatDeskViewPrefs(const DeskViewPrefs& prefs);
// The prefs in `text`, from the defaults; `cardW` is clamped to [cardMin, cardMax].
DeskViewPrefs ParseDeskViewPrefs(const std::string& text, float cardMin, float cardMax);

// Desktop selection, as File Explorer's: a click on `ids[at]` (the files in grid order) with
// Ctrl toggles it and makes it the anchor; with Shift selects the run from the anchor to it
// (the anchor stays; with Ctrl too the run is added, else it replaces the selection; no anchor
// in `ids`: the clicked one); with neither, it alone (the anchor). `anchor` is an id, so it
// survives the grid being rebuilt.
void ClickSelect(const std::vector<int64_t>& ids, size_t at, bool ctrl, bool shift, int64_t& anchor,
                 std::unordered_set<int64_t>& selected);

// A date section: `key` orders and tells sections apart, `label` heads it.
struct DateSection {
    int64_t key = 0;
    std::string label;  // "Today", "Yesterday", or the month, "October 2026"
};

// The section of an item dated `dateMs`, seen at `nowMs` (both ms since the epoch), in the
// local time `utcOffsetSeconds` ahead of UTC. An item from the future counts as today.
DateSection DateBucket(int64_t dateMs, int64_t nowMs, int64_t utcOffsetSeconds);

// Where the tiles of a grid in sections sit: each section starts a new row, under an
// optional header. Content coordinates: x from the grid's left, y from its top.
class GridLayout {
public:
    // `sections`: the tiles in each section, in order. With `headers`, each section has a
    // header `headerHeight` tall above its first row. Tiles are `tile` wide and `tileHeight`
    // tall (< 0: square; the desktop's have a caption under the picture).
    void Build(const std::vector<size_t>& sections, bool headers, int columns, float tile, float gutter,
               float headerHeight, float tileHeight = -1.0f);

    size_t Tiles() const { return tiles_; }
    float Height() const { return height_; }
    // Tile `p`'s top left.
    bool TilePos(size_t p, float& x, float& y) const;
    // The tile at (x, y); false over a gutter or a header, or past the last tile.
    bool TileAt(float x, float y, size_t& p) const;
    // The tiles in rows that reach into [y0, y1): [first, end).
    void Range(float y0, float y1, size_t& first, size_t& end) const;
    // The first tile of the row at the top of a view scrolled to `offset`, and the offset that
    // puts tile `p`'s row (or its section's header, for a section's first row) at the top.
    size_t TileAtOffset(float offset) const;
    float OffsetOf(size_t p) const;

    // The section headers: how many, and section `s`'s header top.
    size_t Headers() const { return headerY_.size(); }
    float HeaderY(size_t s) const { return headerY_[s]; }

private:
    struct Row {
        float y = 0.0f;
        size_t first = 0;  // its first tile
        size_t count = 0;
        bool sectionStart = false;
    };
    int RowOf(size_t p) const;

    std::vector<Row> rows_;
    std::vector<float> headerY_;
    int columns_ = 1;
    float tile_ = 0.0f, step_ = 0.0f, header_ = 0.0f;
    float tileH_ = 0.0f, stepY_ = 0.0f;  // rows: the tile's height and the row pitch
    size_t tiles_ = 0;
    float height_ = 0.0f;
};

}  // namespace mp::browser
