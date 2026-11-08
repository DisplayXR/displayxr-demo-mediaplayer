// SPDX-License-Identifier: Apache-2.0
#include "browser/ThumbnailCache.h"

namespace mp::browser {

void ThumbnailCache::Reset(int slots) {
    slots_.assign(slots > 0 ? (size_t)slots : 0, Slot{});
    byId_.clear();
}

bool ThumbnailCache::Use(int64_t id, int& slot) {
    const auto it = byId_.find(id);
    if (it == byId_.end()) return false;
    slot = it->second;
    slots_[slot].drawn = frame_;
    return true;
}

bool ThumbnailCache::Assign(int64_t id, int& slot) {
    if (Use(id, slot)) return true;
    int best = -1;
    for (int i = 0; i < (int)slots_.size(); ++i) {
        const Slot& s = slots_[i];
        if (!s.filled) {
            best = i;
            break;
        }
        if (s.drawn < frame_ && (best < 0 || s.drawn < slots_[best].drawn)) best = i;
    }
    if (best < 0) return false;
    Slot& s = slots_[best];
    if (s.filled) byId_.erase(s.id);
    s = Slot{true, id, frame_};
    byId_[id] = best;
    slot = best;
    return true;
}

void ThumbnailCache::Remove(int64_t id) {
    const auto it = byId_.find(id);
    if (it == byId_.end()) return;
    slots_[it->second] = Slot{};
    byId_.erase(it);
}

} // namespace mp::browser
