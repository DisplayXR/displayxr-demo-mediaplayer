// SPDX-License-Identifier: Apache-2.0
#include "pro/DisplayCalibration.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace mp::pro::cal {

namespace fs = std::filesystem;

std::string SanitizeKey(const std::string& key) {
    std::string out;
    for (char c : key) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_';
        out += ok ? c : '_';
    }
    // "\\.\DISPLAY1" -> "____DISPLAY1": drop the leading run the device prefix leaves.
    const size_t first = out.find_first_not_of('_');
    if (first == std::string::npos) return "default";
    return out.substr(first);
}

std::string PanelKey(const std::string& runtimeKey, const std::string& deviceName) {
    // The runtime's key is file-safe by contract and is kept as it is (it may carry '@',
    // "AUO-B194@HDMI-1"), so the file name matches `displayxr-cli displays --claims`.
    // Only a key that would leave the folder is sanitized.
    if (!runtimeKey.empty())
        return runtimeKey.find_first_of("/\\:*?\"<>|") == std::string::npos && runtimeKey != "." && runtimeKey != ".."
                   ? runtimeKey
                   : SanitizeKey(runtimeKey);
    return SanitizeKey(deviceName);
}

std::string CalibrationDir() {
    if (const char* o = std::getenv("MEDIAPLAYER_PRO_CAL_DIR"); o && *o) return o;
#if defined(_WIN32)
    PWSTR w = nullptr;
    std::string dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &w)))
        dir = (fs::path(w) / "DisplayXR" / "MediaPlayerPro" / "calibration").u8string();
    CoTaskMemFree(w);
    return dir;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Application Support/DisplayXR/MediaPlayerPro/calibration" : "";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/displayxr/mediaplayer-pro/calibration";
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.config/displayxr/mediaplayer-pro/calibration" : "";
#endif
}

std::string CalibrationPathFor(const std::string& key) {
    const std::string dir = CalibrationDir();
    if (dir.empty()) return {};
    return (fs::u8path(dir) / (PanelKey(key, "") + ".cube")).u8string();  // PanelKey: kept as is unless unsafe
}

bool Install(const std::string& cube, const std::string& key, std::string& err) {
    const std::string dst = CalibrationPathFor(key);
    if (dst.empty()) { err = "no calibration folder"; return false; }
    std::error_code ec;
    const fs::path src = fs::u8path(cube), to = fs::u8path(dst);
    if (fs::equivalent(src, to, ec)) return true;  // already the installed file
    ec.clear();
    fs::create_directories(to.parent_path(), ec);
    if (ec) { err = "cannot create " + to.parent_path().u8string() + ": " + ec.message(); return false; }
    // Through a temporary: a full disk or a crash must not leave a truncated file where the
    // previous calibration was.
    fs::path tmp = to;
    tmp += ".tmp";
    std::error_code ignored;
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(tmp, to, ec);
    if (ec) {
        fs::remove(tmp, ignored);
        err = "cannot copy to " + dst + ": " + ec.message();
        return false;
    }
    return true;
}

bool Remove(const std::string& key, std::string& err) {
    const std::string p = CalibrationPathFor(key);
    if (p.empty()) return true;
    std::error_code ec;
    fs::remove(fs::u8path(p), ec);
    if (ec) { err = "cannot remove " + p + ": " + ec.message(); return false; }
    return true;
}

}  // namespace mp::pro::cal
