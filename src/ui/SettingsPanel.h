// SPDX-License-Identifier: Apache-2.0
//
// SettingsPanel (#265): the one "Settings" window, shared by both products. It draws only
// the rows the product registered: the trunk adds the basic player's rows, Pro's
// ProController adds its own. Rows are grouped under collapsing headers, in the order the
// groups first appear. Esc or the close button closes it.
//
// While it is open the owner keeps the player's hotkeys off (the window has the keyboard);
// once closed it takes nothing.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace mp {
class Settings;
}

namespace mp::ui {

class SettingsPanel {
public:
    void SetStore(Settings* store) { store_ = store; }

    // A checkbox bound to `key`. `enabled` (optional) greys the row out when it returns
    // false; `note` is shown beside a greyed row ("Windows only").
    void AddToggle(const std::string& group, const std::string& label, const std::string& key, bool def,
                   const std::string& tooltip = {}, std::function<bool()> enabled = {},
                   const std::string& note = {});
    // Any row the product draws itself (ImGui calls between the group's other rows).
    void AddCustom(const std::string& group, std::function<void()> draw);

    void Open() { open_ = true; }
    void Close() { open_ = false; }
    void Toggle() { open_ = !open_; }
    bool IsOpen() const { return open_; }

    // Draws the window while it is open (call once per ImGui frame). True while open; `rect`
    // gets its screen rect (x, y, w, h in HUD pixels) for a flat (2D) submission.
    bool Build(float rect[4]);

private:
    struct Row {
        std::string group;
        std::function<void()> draw;
    };
    std::vector<Row> rows_;
    Settings* store_ = nullptr;
    bool open_ = false;
    bool justOpened_ = false;
    bool wasOpen_ = false;
};

}  // namespace mp::ui
