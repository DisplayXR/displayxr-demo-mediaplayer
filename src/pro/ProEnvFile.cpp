// SPDX-License-Identifier: Apache-2.0
#include "pro/ProEnvFile.h"

#include "Log.h"
#include "pro/CaptureLevers.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace mp::pro {

std::string ProEnvFilePath() {
    if (const char* o = std::getenv("MEDIAPLAYER_PRO_ENV_FILE")) {
        const std::string v = o;
        return (v.empty() || v == "off" || v == "0") ? std::string() : v;
    }
#if defined(_WIN32)
    PWSTR w = nullptr;
    std::string path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &w)))
        path = (fs::path(w) / "DisplayXR" / "MediaPlayerPro" / "pro.env").u8string();
    CoTaskMemFree(w);
    return path;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Application Support/DisplayXR/MediaPlayerPro/pro.env" : "";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/displayxr/mediaplayer-pro/pro.env";
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.config/displayxr/mediaplayer-pro/pro.env" : "";
#endif
}

void LoadProEnvFile() {
    const std::string path = ProEnvFilePath();
    if (path.empty()) return;
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return;  // no file: nothing to do
    std::stringstream ss;
    ss << in.rdbuf();
    std::vector<int> bad;
    const std::vector<EnvEntry> entries = ParseEnvText(ss.str(), &bad);
    const std::vector<EnvApplied> applied = ApplyEnvEntries(
        entries, [](const char* k) { return std::getenv(k); },
        [](const std::string& k, const std::string& v) {
#if defined(_WIN32)
            _putenv_s(k.c_str(), v.c_str());
#else
            setenv(k.c_str(), v.c_str(), 1);
#endif
        });
    LOG_INFO("Pro: settings file %s: %zu variable(s)", path.c_str(), applied.size());
    for (const EnvApplied& a : applied)
        LOG_INFO("Pro:   %s=%s%s", a.key.c_str(), a.value.c_str(), a.fromFile ? "" : " (already set in the environment; the file's value ignored)");
    for (int line : bad) LOG_WARN("Pro: %s line %d is not KEY=VALUE; ignored", path.c_str(), line);
}

}  // namespace mp::pro
