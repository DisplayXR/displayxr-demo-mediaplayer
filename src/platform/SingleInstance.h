// SPDX-License-Identifier: Apache-2.0
//
// One player per user session (Windows). Two copies on one panel fight over it: each runs
// its own in-process session, and the second stalled the first for 10 s and more. So a
// second launch hands its file (Explorer's "Open with", a drop on the shortcut) to the
// running player, raises its window, and exits.
//
// MEDIAPLAYER_MULTI_INSTANCE=1 skips the check (tests that run beside a player). The basic
// player and Pro each have their own lock. Elsewhere: no-ops that always claim.
#pragma once

#include <string>
#include <vector>

namespace mp::single_instance {

// True: this is the only player (it now owns the lock). False: another one runs.
bool Claim(bool pro);

// Hands `path` (may be empty: just raise the window) to the running player. Waits up to
// `waitMs` for it to start listening (it may still be starting). True if delivered.
bool Forward(const std::string& path, int waitMs);

// Starts receiving forwarded launches (call once the main window exists).
void Listen();

// Launches forwarded since the last call: their paths ("" = none, raise only). False if none.
bool Take(std::vector<std::string>& paths);

}  // namespace mp::single_instance
