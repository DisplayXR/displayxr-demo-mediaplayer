// SPDX-License-Identifier: Apache-2.0
#include "pro/DeckLinkSpec.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mp {

bool DeckLinkConfig::ParseStereo(const std::string& s, Stereo& out) {
    if (s.empty() || s == "auto") out = Stereo::Auto;
    else if (s == "mono") out = Stereo::Mono;
    else if (s == "sbs") out = Stereo::Sbs;
    else if (s == "dual") out = Stereo::Dual;
    else if (s == "pair") out = Stereo::Pair;
    else return false;
    return true;
}

const char* DeckLinkConfig::StereoName(Stereo s) {
    switch (s) {
        case Stereo::Auto: return "auto";
        case Stereo::Mono: return "mono";
        case Stereo::Sbs: return "sbs";
        case Stereo::Dual: return "dual";
        case Stereo::Pair: return "pair";
    }
    return "?";
}

}  // namespace mp

namespace mp::pro {

bool ParseDeckLinkSpec(const std::string& args, DeckLinkConfig& c) {
    c = DeckLinkConfig{};
    std::string head = args, rest;
    const size_t comma = args.find(',');
    if (comma != std::string::npos) {
        head = args.substr(0, comma);
        rest = args.substr(comma + 1);
    }
    const size_t plus = head.find('+');
    c.device = head.substr(0, plus);
    if (c.device.empty()) c.device = "0";
    if (plus != std::string::npos) {
        c.rightDevice = head.substr(plus + 1);
        c.stereo = DeckLinkConfig::Stereo::Pair;
    }
    size_t pos = 0;
    while (!rest.empty() && pos <= rest.size()) {
        const size_t e = rest.find(',', pos);
        const std::string t = rest.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (t == "sdi" || t == "hdmi") c.input = t;
        else if (t.rfind("mode=", 0) == 0) c.mode = t.substr(5);
        else if (!t.empty()) {
            DeckLinkConfig::Stereo st;
            if (!DeckLinkConfig::ParseStereo(t, st)) return false;
            c.stereo = st;
        }
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return true;
}

namespace {
std::string Lower(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}
}  // namespace

int DeckLinkDialogDevice(const std::string& selector, const std::vector<std::string>& devices) {
    char* end = nullptr;
    const long index = std::strtol(selector.c_str(), &end, 10);
    if (!selector.empty() && end && *end == '\0')
        return (index >= 0 && index < (long)devices.size()) ? (int)index : -1;
    if (selector.empty()) return -1;
    const std::string want = Lower(selector);
    for (size_t i = 0; i < devices.size(); ++i) {
        // Match the name only, not the list's "N: " index prefix.
        const size_t colon = devices[i].find(": ");
        const std::string name = colon == std::string::npos ? devices[i] : devices[i].substr(colon + 2);
        if (Lower(name).find(want) != std::string::npos) return (int)i;
    }
    return -1;
}

bool DeckLinkDialogFromConfig(const DeckLinkConfig& c, const std::vector<std::string>& devices,
                              DeckLinkDialogFields& out) {
    using S = DeckLinkConfig::Stereo;
    DeckLinkDialogFields f;
    f.left = DeckLinkDialogDevice(c.device, devices);
    if (f.left < 0) return false;
    if (c.stereo == S::Pair) {
        const int r = DeckLinkDialogDevice(c.rightDevice, devices);
        if (r < 0) return false;
        f.right = r + 1;
    }
    f.conn = c.input == "sdi" ? 1 : c.input == "hdmi" ? 2 : 0;
    f.stereo = c.stereo == S::Sbs ? 1 : c.stereo == S::Dual ? 2 : c.stereo == S::Mono ? 3 : 0;
    f.mode = c.mode;
    out = f;
    return true;
}

std::string DeckLinkSpecFromDialog(const DeckLinkDialogFields& f) {
    const bool pair = f.right > 0;
    std::string spec = std::to_string(f.left);
    if (pair) spec += "+" + std::to_string(f.right - 1);
    if (f.conn == 1) spec += ",sdi";
    if (f.conn == 2) spec += ",hdmi";
    if (!pair) {
        const char* st[] = {"", "sbs", "dual", "mono"};
        if (f.stereo >= 1 && f.stereo <= 3) spec += std::string(",") + st[f.stereo];
    }
    if (!f.mode.empty()) spec += ",mode=" + f.mode;
    return spec;
}

}  // namespace mp::pro
