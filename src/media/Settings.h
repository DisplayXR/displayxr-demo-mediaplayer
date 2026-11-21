// SPDX-License-Identifier: Apache-2.0
//
// Settings (#265): the player's per-user preferences, one "key=value" a line in
// settings.txt beside recent.txt (RecentListPath()'s folder, so one file per product).
// Hand-editable, like browser_view.txt and Pro's pro.env.
//
// Lines it does not know are kept, in order: comments, blank lines and keys a newer build
// wrote all survive a save from an older build (basic and Pro, or two versions, may share a
// box). A key that appears twice keeps its first place and its last value.
//
// The getters take the default and, for numbers, the allowed range: a missing key or a bad
// value gives the default, an out-of-range number is clamped. Setters mark the store dirty;
// Save() writes only when something changed, through a sibling .tmp and a rename.
//
// pro.env is NOT this file: it sets environment variables before anything reads them (a
// restart applies it). settings.txt holds live preferences the UI switches.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mp {

class Settings {
public:
    // Reads `file` and remembers it as the place Save() writes. A missing file is an empty
    // store (returns false, the path is still kept); an empty path is a memory-only store.
    bool Load(const std::string& file);
    // Writes to the loaded path when dirty. True when the file is current (nothing to do
    // counts); false on an I/O error or a memory-only store with changes.
    bool Save();
    const std::string& Path() const { return path_; }

    void SetFromText(const std::string& text);
    std::string ToText() const;

    bool Has(const std::string& key) const;
    std::string GetString(const std::string& key, const std::string& def) const;
    // 1 / 0, true / false, on / off, yes / no (any case); anything else is `def`.
    bool GetBool(const std::string& key, bool def) const;
    float GetFloat(const std::string& key, float def, float lo, float hi) const;
    int GetInt(const std::string& key, int def, int lo, int hi) const;

    void SetString(const std::string& key, const std::string& value);
    void SetBool(const std::string& key, bool value) { SetString(key, value ? "1" : "0"); }
    void SetFloat(const std::string& key, float value);
    void SetInt(const std::string& key, int value) { SetString(key, std::to_string(value)); }

    bool Dirty() const { return dirty_; }
    // Bumped by every change: the owner debounces its saves on it.
    uint64_t Generation() const { return generation_; }

private:
    struct Line {
        std::string text;  // the line as read: a comment, a blank, or text without '='
        std::string key, value;
        bool isKey = false;
    };
    const Line* Find(const std::string& key) const;

    std::vector<Line> lines_;
    std::string path_;
    bool dirty_ = false;
    uint64_t generation_ = 0;
};

// settings.txt for `appDir` ("MediaPlayer" / "MediaPlayerPro"): beside RecentListPath(),
// empty when there is none. MEDIAPLAYER_SETTINGS_FILE overrides it ("" / "off" = memory only).
std::string SettingsPath(const char* appDir);

} // namespace mp
