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

std::string PanelKeyFromEdid(const uint8_t* e, size_t size) {
    static const uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (!e || size < 128) return {};
    for (int i = 0; i < 8; ++i)
        if (e[i] != kHeader[i]) return {};

    // Manufacturer: three 5-bit letters, big-endian in bytes 8-9 ('A' = 1).
    const unsigned m = ((unsigned)e[8] << 8) | e[9];
    char pnp[4] = {(char)('A' - 1 + ((m >> 10) & 31)), (char)('A' - 1 + ((m >> 5) & 31)),
                   (char)('A' - 1 + (m & 31)), 0};
    for (int i = 0; i < 3; ++i)
        if (pnp[i] < 'A' || pnp[i] > 'Z') pnp[i] = '_';
    const unsigned prod = (unsigned)e[10] | ((unsigned)e[11] << 8);  // little-endian
    char head[16];
    std::snprintf(head, sizeof(head), "%s-%04X", pnp, prod);

    // The serial string descriptor (tag 0xFF) in one of the four 18-byte descriptors.
    for (int d = 0; d < 4; ++d) {
        const uint8_t* p = e + 54 + d * 18;
        if (p[0] == 0 && p[1] == 0 && p[3] == 0xFF) {
            std::string s;
            for (int i = 5; i < 18 && p[i] != 0x0A && p[i] != 0; ++i) s += (char)p[i];
            while (!s.empty() && s.back() == ' ') s.pop_back();
            if (!s.empty()) return SanitizeKey(std::string(head) + "-" + s);
        }
    }
    const uint32_t serial = (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) |
                            ((uint32_t)e[15] << 24);
    if (serial != 0) return std::string(head) + "-" + std::to_string(serial);
    return head;
}

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

#if defined(_WIN32)
// The EDID of the monitor on GDI device `gdi` (\\.\DISPLAY1). EnumDisplayDevices with
// EDD_GET_DEVICE_INTERFACE_NAME gives the monitor's interface path,
//   \\?\DISPLAY#ACR0000#5&2a1b3c&0&UID4353#{e6f07b5f-...}
// whose middle two parts are its key under HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY;
// "Device Parameters\EDID" there is readable without admin rights.
static bool ReadEdid(const std::string& gdi, std::string& key) {
    DISPLAY_DEVICEA mon = {};
    mon.cb = sizeof(mon);
    for (DWORD i = 0; EnumDisplayDevicesA(gdi.c_str(), i, &mon, EDD_GET_DEVICE_INTERFACE_NAME); ++i) {
        if (!(mon.StateFlags & DISPLAY_DEVICE_ACTIVE)) { mon.cb = sizeof(mon); continue; }
        std::string id = mon.DeviceID;  // \\?\DISPLAY#ACR0000#5&...&UID4353#{guid}
        const size_t a = id.find('#');
        const size_t b = a == std::string::npos ? a : id.find('#', a + 1);
        const size_t c = b == std::string::npos ? b : id.find('#', b + 1);
        if (c == std::string::npos) return false;
        const std::string sub = "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + id.substr(a + 1, b - a - 1) + "\\" +
                                id.substr(b + 1, c - b - 1) + "\\Device Parameters";
        uint8_t edid[512];
        DWORD n = sizeof(edid);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, sub.c_str(), "EDID", RRF_RT_REG_BINARY, nullptr, edid, &n) !=
            ERROR_SUCCESS)
            return false;
        key = PanelKeyFromEdid(edid, n);
        return !key.empty();
    }
    return false;
}
#endif

std::string PanelKeyForDevice(const std::string& deviceName) {
    if (deviceName.empty()) return "default";
#if defined(_WIN32)
    std::string key;
    if (ReadEdid(deviceName, key)) return key;
#endif
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
    return (fs::u8path(dir) / (SanitizeKey(key) + ".cube")).u8string();
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
    fs::copy_file(src, to, fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "cannot copy to " + dst + ": " + ec.message(); return false; }
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
