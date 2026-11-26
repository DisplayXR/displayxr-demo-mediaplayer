// SPDX-License-Identifier: Apache-2.0
#include "xr/DisplayModeSwitch.h"

#include <utility>

namespace mp {

// The two-mode contract by POSITION (runtime #2766, display_info v27): exactly two modes,
// index 0 (one view) and 1 — then 0 is the 2D mode and 1 the 3D mode whatever their content. Slot 1
// may be a one-view "Same as 2D" entry with the lens off, so under this contract the 3D
// mode is never looked up by view count or the hardware flag.
static bool Positional(const std::vector<DisplayModeSwitch::Mode>& modes) {
    if (modes.size() != 2) return false;
    bool has0 = false, has1 = false;
    for (const auto& m : modes) {
        has0 = has0 || (m.index == 0 && m.viewCount == 1);
        has1 = has1 || m.index == 1;
    }
    return has0 && has1;
}

void DisplayModeSwitch::SetModes(std::vector<Mode> modes) {
    modes_ = std::move(modes);
    is3D_ = false;
    last3D_ = -1;
}

void DisplayModeSwitch::SetActive(uint32_t index) {
    is3D_ = false;
    if (Positional(modes_)) {
        is3D_ = index == 1;
    } else {
        for (const Mode& m : modes_)
            if (m.index == index) is3D_ = m.viewCount > 1;
    }
    if (is3D_) last3D_ = (int32_t)index;
}

int32_t DisplayModeSwitch::Flat() const {
    if (Positional(modes_)) return 0;
    int32_t fallback = -1;
    for (const Mode& m : modes_) {
        if (m.viewCount != 1) continue;
        if (!m.hardware3D) return (int32_t)m.index;
        if (fallback < 0) fallback = (int32_t)m.index;
    }
    return fallback;
}

int32_t DisplayModeSwitch::Stereo() const {
    if (Positional(modes_)) return 1;
    if (last3D_ >= 0) return last3D_;
    int32_t fallback = -1;
    for (const Mode& m : modes_) {
        if (m.viewCount < 2) continue;
        if (m.hardware3D) return (int32_t)m.index;
        if (fallback < 0) fallback = (int32_t)m.index;
    }
    return fallback;
}

} // namespace mp
