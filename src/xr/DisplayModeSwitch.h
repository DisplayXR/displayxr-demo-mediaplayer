// SPDX-License-Identifier: Apache-2.0
//
// DisplayModeSwitch: the player's 2D/3D switch over the runtime's rendering modes
// (XR_DXR_display_info). 2D is the flat mode; 3D is the stereo mode last in force, else
// the first that turns the lens on. Pure logic.
#pragma once

#include <cstdint>
#include <vector>

namespace mp {

class DisplayModeSwitch {
public:
    struct Mode {
        uint32_t index = 0;
        uint32_t viewCount = 1;
        bool hardware3D = false;
    };

    void SetModes(std::vector<Mode> modes);
    // The mode the runtime reports active, at start and on every mode change.
    void SetActive(uint32_t index);

    // A 2D and a 3D mode both exist.
    bool Available() const { return Flat() >= 0 && Stereo() >= 0; }
    bool Is3D() const { return is3D_; }
    // The mode a flip asks for: 2D from 3D and 3D from 2D; -1 if there is none.
    int32_t Flipped() const { return is3D_ ? Flat() : Stereo(); }
    // Desktop rule (XrSession::FindFlatMode): mono and lens off first; a mono mode that
    // leaves the lens on still beats a stereo one; -1 when nothing is flat.
    int32_t Flat() const;
    // The 3D mode a flip from 2D asks for: the one last in force, else the first that turns
    // the lens on; -1 when there is none. (The browser's 3D grid borrows it too, #180.)
    int32_t Stereo() const;

private:

    std::vector<Mode> modes_;
    bool is3D_ = false;
    int32_t last3D_ = -1;
};

} // namespace mp
