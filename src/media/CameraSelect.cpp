// SPDX-License-Identifier: Apache-2.0
#include "media/CameraSelect.h"

#include <cctype>
#include <cstdlib>

namespace mp {

namespace {
std::string Lower(const std::string& s) {
    std::string o(s);
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Rank a format's rate: exact wantFps first, then the highest <= 60, then anything above.
int RateScore(int fps, int wantFps) {
    if (wantFps > 0 && fps == wantFps) return 1000000;
    if (fps <= 60) return 1000 + fps;
    return 1000 - fps;
}

int Tier(const CameraFormat& f) { return f.nv12 ? 2 : (f.mjpg ? 0 : 1); }
}  // namespace

int FormatFps(const CameraFormat& f) {
    if (f.fpsDen <= 0) return 0;
    return (int)((f.fpsNum + f.fpsDen / 2) / f.fpsDen);
}

bool IsDeniedCameraName(const std::string& name, const std::string& extraDenyEnv) {
    static const char* const kBuiltins[] = {"tracking", "ir camera", "infrared", "depth"};
    const std::string n = Lower(name);
    for (const char* b : kBuiltins)
        if (n.find(b) != std::string::npos) return true;
    const std::string extra = Lower(extraDenyEnv);
    size_t start = 0;
    while (start <= extra.size()) {
        size_t end = extra.find(';', start);
        if (end == std::string::npos) end = extra.size();
        std::string tok = extra.substr(start, end - start);
        while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        if (!tok.empty() && n.find(tok) != std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

int DefaultCamera(const std::vector<CameraDevice>& devs) {
    for (size_t i = 0; i < devs.size(); ++i)
        if (!devs[i].denied && Lower(devs[i].name).find("spatiallabs eyes") != std::string::npos)
            return (int)i;
    for (size_t i = 0; i < devs.size(); ++i) {
        if (devs[i].denied) continue;
        for (const CameraFormat& f : devs[i].formats)
            if (f.w >= 3840) return (int)i;
    }
    for (size_t i = 0; i < devs.size(); ++i)
        if (!devs[i].denied) return (int)i;
    return -1;
}

int SelectCamera(const std::vector<CameraDevice>& devs, const std::string& selector) {
    const std::string sel = Lower(selector);
    if (sel.empty() || sel == "auto") return DefaultCamera(devs);
    if (AllDigits(sel)) {
        const long want = std::strtol(sel.c_str(), nullptr, 10);
        long k = 0;
        for (size_t i = 0; i < devs.size(); ++i) {
            if (devs[i].denied) continue;
            if (k++ == want) return (int)i;
        }
        return -1;
    }
    for (size_t i = 0; i < devs.size(); ++i)
        if (!devs[i].denied && Lower(devs[i].name).find(sel) != std::string::npos) return (int)i;
    return -1;
}

bool ChooseFormat(const CameraDevice& d, int wantW, int wantH, int wantFps, CameraFormat& out,
                  bool allowMjpg) {
    bool exactSize = false;
    if (wantW > 0 && wantH > 0) {
        for (const CameraFormat& f : d.formats)
            if ((allowMjpg || !f.mjpg) && f.w == wantW && f.h == wantH) exactSize = true;
    }
    const CameraFormat* best = nullptr;
    long long bestArea = -1;
    int bestRate = 0, bestTier = -1;
    for (const CameraFormat& f : d.formats) {
        if (!allowMjpg && f.mjpg) continue;
        if (f.w <= 0 || f.h <= 0) continue;
        if (exactSize && (f.w != wantW || f.h != wantH)) continue;
        const long long area = (long long)f.w * f.h;
        const int rate = RateScore(FormatFps(f), wantFps);
        const int tier = Tier(f);
        const bool better = area > bestArea || (area == bestArea && rate > bestRate) ||
                            (area == bestArea && rate == bestRate && tier > bestTier);
        if (better) {
            best = &f;
            bestArea = area;
            bestRate = rate;
            bestTier = tier;
        }
    }
    if (!best) return false;
    out = *best;
    return true;
}

}  // namespace mp
