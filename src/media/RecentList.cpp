// SPDX-License-Identifier: Apache-2.0
#include "media/RecentList.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace mp {

bool RecentList::SamePath(const std::string& a, const std::string& b) {
#if defined(_WIN32)
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (std::tolower((unsigned char)x) != std::tolower((unsigned char)y)) return false;
    }
    return true;
#else
    return a == b;
#endif
}

bool RecentList::Add(const std::string& path) {
    if (path.empty()) return false;
    if (!items_.empty() && items_.front() == path) return false;
    Remove(path);
    items_.insert(items_.begin(), path);
    if (items_.size() > capacity_) items_.resize(capacity_);
    return true;
}

bool RecentList::Remove(const std::string& path) {
    const auto it = std::remove_if(items_.begin(), items_.end(),
                                   [&](const std::string& p) { return SamePath(p, path); });
    if (it == items_.end()) return false;
    items_.erase(it, items_.end());
    return true;
}

void RecentList::SetFromText(const std::string& text) {
    items_.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line) && items_.size() < capacity_) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        line.erase(0, start);
        if (line.empty()) continue;
        const bool dup = std::any_of(items_.begin(), items_.end(),
                                     [&](const std::string& p) { return SamePath(p, line); });
        if (!dup) items_.push_back(line);
    }
}

std::string RecentList::ToText() const {
    std::string out;
    for (const std::string& p : items_) {
        out += p;
        out += '\n';
    }
    return out;
}

bool RecentList::Load(const std::string& file) {
    if (file.empty()) return false;
    std::ifstream in(fs::u8path(file), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    SetFromText(ss.str());
    return true;
}

bool RecentList::Save(const std::string& file) const {
    if (file.empty()) return false;
    std::error_code ec;
    const fs::path p = fs::u8path(file);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    // Write a sibling then rename, so a crash mid-write never leaves half a list.
    const fs::path tmp = fs::path(p).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << ToText();
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string RecentListPath(const char* appDir) {
    if (const char* o = std::getenv("MEDIAPLAYER_RECENT_FILE")) {
        const std::string v = o;
        return (v.empty() || v == "off" || v == "0") ? std::string() : v;
    }
    const std::string app = appDir ? appDir : "MediaPlayer";
#if defined(_WIN32)
    PWSTR w = nullptr;
    std::string path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &w)))
        path = (fs::path(w) / "DisplayXR" / app / "recent.txt").u8string();
    CoTaskMemFree(w);
    return path;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Application Support/DisplayXR/" + app + "/recent.txt" : "";
#else
    std::string lower = app == "MediaPlayerPro" ? "mediaplayer-pro" : "mediaplayer";
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/displayxr/" + lower + "/recent.txt";
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.config/displayxr/" + lower + "/recent.txt" : "";
#endif
}

std::string RecentLabel(const std::string& path, bool isFolder) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t cut = p.find_last_of("/\\");
    std::string name = cut == std::string::npos ? p : p.substr(cut + 1);
    std::string parent = cut == std::string::npos ? std::string() : p.substr(0, cut);
    if (name.empty()) name = p;  // a drive root ("D:") or "/"
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (isFolder) name += sep;
    if (parent.size() == 2 && parent[1] == ':') parent += sep;  // "D:" -> "D:\"
    return parent.empty() ? name : name + "   (" + parent + ")";
}

} // namespace mp
