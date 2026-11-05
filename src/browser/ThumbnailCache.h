// SPDX-License-Identifier: Apache-2.0
//
// ThumbnailCache: which media item's thumbnail is in which ThumbnailAtlas slot. A new
// thumbnail takes an empty slot, else the one drawn least recently, but never one drawn
// this frame: that would change a tile on screen. Pure logic, render thread.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mp::browser {

class ThumbnailCache {
public:
    // `slots` empty slots. Forgets every thumbnail.
    void Reset(int slots);
    // Start a frame. The slots drawn from here on are kept until the next one.
    void NextFrame() { ++frame_; }

    bool Contains(int64_t id) const { return byId_.count(id) != 0; }
    // The slot with `id`'s thumbnail, now marked drawn this frame. False if it has none.
    bool Use(int64_t id, int& slot);
    // A slot for `id`'s thumbnail, marked drawn this frame: its own, else an empty one,
    // else the least recently drawn one, whose item loses it. False if every slot was
    // drawn this frame.
    bool Assign(int64_t id, int& slot);
    // Empty `id`'s slot.
    void Remove(int64_t id);

private:
    struct Slot {
        bool filled = false;
        int64_t id = 0;
        uint64_t drawn = 0;  // the frame it was last drawn in
    };

    std::vector<Slot> slots_;
    std::unordered_map<int64_t, int> byId_;
    uint64_t frame_ = 1;
};

} // namespace mp::browser
