// SPDX-License-Identifier: Apache-2.0
//
// PerfSwitches - Media Player Pro's A/B switches for the render thread's OPTIONAL per-frame
// work, read once at start from the environment (docs/PRO.md, first check 7). Each is on
// unless its variable is 0 / off / false / no. A station that can only run signed builds
// turns one off, restarts and reads the `perf:` stages again, with no new build. Never the
// source upload or the picture's draw. Pure: tests/render_perf_test.cpp.
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace mp::pro {

struct PerfSwitches {
    bool rig = true;       // MEDIAPLAYER_PRO_RIG: the rig check's Submit / Downsample
    bool thumbs = true;    // MEDIAPLAYER_PRO_THUMBS: the live input's list thumbnail
    bool autoConv = true;  // MEDIAPLAYER_PRO_AUTOCONV: auto-convergence sampling
    bool ui = true;        // MEDIAPLAYER_PRO_UI: the UI (ImGui) while a live source is up

    // The switches that are off ("rig", "thumbs", "autoconv", "ui"): get_rig_status
    // render.switched_off, and joined ("rig, ui") on the `perf:` line. Empty when all are on.
    std::vector<const char*> OffNames() const {
        std::vector<const char*> v;
        if (!rig) v.push_back("rig");
        if (!thumbs) v.push_back("thumbs");
        if (!autoConv) v.push_back("autoconv");
        if (!ui) v.push_back("ui");
        return v;
    }
    std::string Off() const {
        std::string s;
        for (const char* n : OffNames()) s += (s.empty() ? "" : ", ") + std::string(n);
        return s;
    }
};

// A switch's value: 0 / off / false / no (any case) turn it off; anything else is on.
inline bool PerfSwitchOn(const char* v) {
    if (!v) return true;
    std::string s;
    for (const char* p = v; *p; ++p) s += (char)std::tolower((unsigned char)*p);
    return !(s == "0" || s == "off" || s == "false" || s == "no");
}

// Reads the switches through `get(name)` (a getenv; nullptr = unset). `onSet(name, value, on,
// what)` is called for each variable that is set, so the caller logs it once.
template <class Getenv, class OnSet>
PerfSwitches ReadPerfSwitches(Getenv&& get, OnSet&& onSet) {
    PerfSwitches s;
    const auto one = [&](const char* name, bool& flag, const char* what) {
        if (const char* v = get(name)) {
            flag = PerfSwitchOn(v);
            onSet(name, v, flag, what);
        }
    };
    one("MEDIAPLAYER_PRO_RIG", s.rig, "rig check (Submit / Downsample on the render thread)");
    one("MEDIAPLAYER_PRO_THUMBS", s.thumbs, "live input thumbnails");
    one("MEDIAPLAYER_PRO_AUTOCONV", s.autoConv, "auto-convergence sampling");
    one("MEDIAPLAYER_PRO_UI", s.ui, "UI while live");
    return s;
}

}  // namespace mp::pro
