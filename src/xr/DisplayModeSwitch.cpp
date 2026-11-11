// SPDX-License-Identifier: Apache-2.0
#include "xr/DisplayModeSwitch.h"

#include <utility>

namespace mp {

void DisplayModeSwitch::SetModes(std::vector<Mode> modes) {
    modes_ = std::move(modes);
    is3D_ = false;
    last3D_ = -1;
}

void DisplayModeSwitch::SetActive(uint32_t index) {
    is3D_ = false;
    for (const Mode& m : modes_)
        if (m.index == index) is3D_ = m.viewCount > 1;
    if (is3D_) last3D_ = (int32_t)index;
}

int32_t DisplayModeSwitch::Flat() const {
    int32_t fallback = -1;
    for (const Mode& m : modes_) {
        if (m.viewCount != 1) continue;
        if (!m.hardware3D) return (int32_t)m.index;
        if (fallback < 0) fallback = (int32_t)m.index;
    }
    return fallback;
}

int32_t DisplayModeSwitch::Stereo() const {
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
