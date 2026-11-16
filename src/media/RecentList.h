// SPDX-License-Identifier: Apache-2.0
//
// RecentList: the desktop player's "Recent" menu (#252 phase 0). The files and folders the
// user opened on purpose (Open, Open folder, a drop, the command line, a pick from this
// list), newest first, without duplicates, capped. Walking a folder with the arrow keys or
// the slideshow does not add to it: the list remembers gestures, not every file shown.
//
// The list logic is pure. Load / Save read and write a UTF-8 text file, one path per line,
// at RecentListPath(): %APPDATA%\DisplayXR\<app>\recent.txt on Windows,
// ~/Library/Application Support/DisplayXR/<app>/recent.txt on macOS and
// $XDG_CONFIG_HOME (or ~/.config)/displayxr/<app>/recent.txt on Linux. Missing entries are
// kept: a file on an unplugged drive or a cloud placeholder is not gone, and probing them
// here could download a cloud file. A pick that no longer exists is removed then.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mp {

class RecentList {
public:
    static constexpr size_t kDefaultCapacity = 12;

    explicit RecentList(size_t capacity = kDefaultCapacity) : capacity_(capacity ? capacity : 1) {}

    // Moves `path` to the front (adding it if new) and drops the oldest past the capacity.
    // Paths compare case-insensitively with either slash on Windows, exactly elsewhere.
    // Empty paths are ignored. Returns true when the list changed.
    bool Add(const std::string& path);
    bool Remove(const std::string& path);
    void Clear() { items_.clear(); }

    const std::vector<std::string>& Items() const { return items_; }
    bool Empty() const { return items_.empty(); }

    // One path per line; blank lines and duplicates are skipped, the capacity applies.
    void SetFromText(const std::string& text);
    std::string ToText() const;

    // File I/O at `file` (no-ops returning false on an empty path or an I/O error).
    bool Load(const std::string& file);
    bool Save(const std::string& file) const;

    static bool SamePath(const std::string& a, const std::string& b);

private:
    size_t capacity_;
    std::vector<std::string> items_;
};

// Where the list lives for `appDir` ("MediaPlayer" / "MediaPlayerPro"); empty if the
// platform folder cannot be found. MEDIAPLAYER_RECENT_FILE overrides it ("off" = none).
std::string RecentListPath(const char* appDir);

// The menu label for `path`: its file or folder name, then its parent folder in brackets,
// e.g. "trailer.mp4  (D:\Media\Videos)". Folders end with the separator in the name part.
std::string RecentLabel(const std::string& path, bool isFolder);

} // namespace mp
