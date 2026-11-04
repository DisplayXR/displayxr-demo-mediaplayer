// SPDX-License-Identifier: Apache-2.0
#include "pro/TargetScreen.h"

#include <cctype>
#include <cmath>

namespace mp::pro::target {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.0f / kPi;

std::string Key(const std::string& s) {
    std::string k;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) k += (char)std::tolower((unsigned char)c);
    return k;
}
}  // namespace

const std::vector<Screen>& Presets() {
    static const std::vector<Screen> p = {
        {"Cinema", 10.0f, 12.0f},
        {"Home TV", 1.44f, 2.5f},
        {"Monitor", 0.60f, 0.65f},
        {"Tablet", 0.25f, 0.40f},
    };
    return p;
}

bool PresetByName(const std::string& name, Screen& out) {
    std::string k = Key(name);
    if (k == "tv" || k == "hometv") k = "hometv";
    for (const Screen& s : Presets())
        if (Key(s.name) == k) {
            out = s;
            return true;
        }
    return false;
}

float PictureScale(const Screen& t, float wp, float dp) {
    if (!t.On() || wp <= 0.0f || dp <= 0.0f) return 1.0f;
    return (t.widthM / t.distanceM) / (wp / dp);
}

Depth AtParallax(const Screen& t, float pct, float e) {
    Depth d;
    if (!t.On()) return d;
    const float p = pct / 100.0f * t.widthM;  // metres
    d.mm = p * 1000.0f;
    d.arcmin = 2.0f * std::atan(p / (2.0f * t.distanceM)) * kDeg * 60.0f;
    if (p > e) d.divergenceDeg = 2.0f * std::atan((p - e) / (2.0f * t.distanceM)) * kDeg;
    return d;
}

float DivergenceLimitPct(const Screen& t, float capDeg, float e) {
    if (!t.On()) return 0.0f;
    const float p = e + 2.0f * t.distanceM * std::tan(capDeg / kDeg / 2.0f);
    return p / t.widthM * 100.0f;
}

}  // namespace mp::pro::target
