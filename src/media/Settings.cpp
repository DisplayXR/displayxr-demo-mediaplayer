// SPDX-License-Identifier: Apache-2.0
#include "media/Settings.h"

#include "media/RecentList.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace mp {

namespace {

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool ParseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* stop = nullptr;
    const double v = std::strtod(s.c_str(), &stop);
    if (stop == s.c_str() || *stop != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

} // namespace

void Settings::SetFromText(const std::string& text) {
    lines_.clear();
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        Line line;
        line.text = text.substr(at, end - at);
        if (!line.text.empty() && line.text.back() == '\r') line.text.pop_back();
        at = end + 1;
        const std::string t = Trim(line.text);
        const size_t eq = t.find('=');
        if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos && eq > 0) {
            line.key = Trim(t.substr(0, eq));
            line.value = Trim(t.substr(eq + 1));
            line.isKey = !line.key.empty();
        }
        if (line.isKey) {
            // A repeated key keeps its first place and takes the later value.
            auto it = std::find_if(lines_.begin(), lines_.end(),
                                   [&](const Line& l) { return l.isKey && l.key == line.key; });
            if (it != lines_.end()) {
                it->value = line.value;
                continue;
            }
        }
        lines_.push_back(std::move(line));
    }
}

std::string Settings::ToText() const {
    std::string out;
    for (const Line& l : lines_) {
        out += l.isKey ? l.key + "=" + l.value : l.text;
        out += '\n';
    }
    return out;
}

const Settings::Line* Settings::Find(const std::string& key) const {
    for (const Line& l : lines_)
        if (l.isKey && l.key == key) return &l;
    return nullptr;
}

bool Settings::Has(const std::string& key) const { return Find(key) != nullptr; }

std::string Settings::GetString(const std::string& key, const std::string& def) const {
    const Line* l = Find(key);
    return l ? l->value : def;
}

bool Settings::GetBool(const std::string& key, bool def) const {
    const Line* l = Find(key);
    if (!l) return def;
    std::string v = l->value;
    for (char& c : v) c = (char)std::tolower((unsigned char)c);
    if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "off" || v == "no") return false;
    return def;
}

float Settings::GetFloat(const std::string& key, float def, float lo, float hi) const {
    const Line* l = Find(key);
    double v = 0.0;
    if (!l || !ParseNumber(l->value, v)) return def;
    return std::min(hi, std::max(lo, (float)v));
}

int Settings::GetInt(const std::string& key, int def, int lo, int hi) const {
    const Line* l = Find(key);
    double v = 0.0;
    if (!l || !ParseNumber(l->value, v)) return def;
    return (int)std::lround(std::min((double)hi, std::max((double)lo, v)));
}

void Settings::SetString(const std::string& key, const std::string& value) {
    const std::string k = Trim(key), v = Trim(value);
    if (k.empty() || k.find('=') != std::string::npos || k[0] == '#' || k[0] == ';') return;
    if (v.find('\n') != std::string::npos) return;  // one line per key
    for (Line& l : lines_) {
        if (l.isKey && l.key == k) {
            if (l.value == v) return;
            l.value = v;
            dirty_ = true;
            ++generation_;
            return;
        }
    }
    Line l;
    l.key = k;
    l.value = v;
    l.isKey = true;
    lines_.push_back(std::move(l));
    dirty_ = true;
    ++generation_;
}

void Settings::SetFloat(const std::string& key, float value) {
    if (!std::isfinite(value)) return;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", value);
    SetString(key, buf);
}

bool Settings::Load(const std::string& file) {
    path_ = file;
    lines_.clear();
    dirty_ = false;
    if (file.empty()) return false;
    std::ifstream in(fs::u8path(file), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    SetFromText(ss.str());
    return true;
}

bool Settings::Save() {
    if (!dirty_) return true;
    if (path_.empty()) return false;
    std::error_code ec;
    const fs::path p = fs::u8path(path_);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::string text = ToText();
    if (lines_.empty() || lines_.front().isKey)  // a new file says what it is
        text = "# DisplayXR Media Player settings (key=value). Unknown keys are kept.\n" + text;
    // A sibling then a rename, so a crash mid-write never leaves half a file.
    const fs::path tmp = fs::path(p).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    if (lines_.empty() || lines_.front().isKey) SetFromText(text);  // keep the header in memory too
    dirty_ = false;
    return true;
}

std::string SettingsPath(const char* appDir) {
    if (const char* o = std::getenv("MEDIAPLAYER_SETTINGS_FILE")) {
        const std::string v = o;
        return (v.empty() || v == "off" || v == "0") ? std::string() : v;
    }
    const std::string recent = RecentListPath(appDir);
    if (recent.empty()) return std::string();
    return (fs::u8path(recent).parent_path() / "settings.txt").u8string();
}

} // namespace mp
