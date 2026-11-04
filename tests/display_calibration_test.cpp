// SPDX-License-Identifier: Apache-2.0
// Per-panel display calibration (#133): the panel key, and the file it maps to.
#include "pro/DisplayCalibration.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static int g_fail = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                           \
        }                                                                       \
    } while (0)

using namespace mp::pro::cal;

int main() {
    // The runtime's stable key (XrDisplayStableKeyDXR) is used as it is, '@' included, so
    // the file name matches `displayxr-cli displays --claims`.
    CHECK(PanelKey("AUO-QALA2137AL0011", "\\\\.\\DISPLAY1") == "AUO-QALA2137AL0011");
    CHECK(PanelKey("AUO-B194@HDMI-1", "HDMI-1") == "AUO-B194@HDMI-1");
    CHECK(PanelKey("ACR-0000-871F0C57", "") == "ACR-0000-871F0C57");
    // A key that would leave the folder is sanitized instead.
    CHECK(PanelKey("../evil", "") == "evil");
    CHECK(PanelKey("a\\b:c", "") == "a_b_c");
    CHECK(PanelKey("..", "") == "default");
    // No key (a runtime older than display_info v24): the OS device name stands in.
    CHECK(PanelKey("", "\\\\.\\DISPLAY1") == "DISPLAY1");
    CHECK(PanelKey("", "HDMI-1") == "HDMI-1");
    CHECK(PanelKey("", "") == "default");

    CHECK(SanitizeKey("\\\\.\\DISPLAY1") == "DISPLAY1");
    CHECK(SanitizeKey("") == "default");
    CHECK(SanitizeKey("///") == "default");

    // The calibration folder honours MEDIAPLAYER_PRO_CAL_DIR; Install / Remove round-trip.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dxr_cal_test";
    std::filesystem::remove_all(dir);
#if defined(_WIN32)
    _putenv_s("MEDIAPLAYER_PRO_CAL_DIR", dir.string().c_str());
#else
    setenv("MEDIAPLAYER_PRO_CAL_DIR", dir.string().c_str(), 1);
#endif
    CHECK(CalibrationDir() == dir.string());
    CHECK(CalibrationPathFor("AUO-B194@HDMI-1") == (dir / "AUO-B194@HDMI-1.cube").string());
    const std::filesystem::path src = std::filesystem::temp_directory_path() / "dxr_cal_src.cube";
    { std::ofstream(src) << "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"; }
    std::string err;
    CHECK(Install(src.string(), "AUO-B194@HDMI-1", err));
    CHECK(std::filesystem::exists(dir / "AUO-B194@HDMI-1.cube"));
    CHECK(Install((dir / "AUO-B194@HDMI-1.cube").string(), "AUO-B194@HDMI-1", err));  // the installed file itself
    CHECK(Remove("AUO-B194@HDMI-1", err));
    CHECK(!std::filesystem::exists(dir / "AUO-B194@HDMI-1.cube"));
    CHECK(Remove("AUO-B194@HDMI-1", err));  // nothing to remove is fine
    std::filesystem::remove_all(dir);
    std::filesystem::remove(src);

    if (g_fail) std::fprintf(stderr, "display_calibration_test: %d failure(s)\n", g_fail);
    else std::printf("display_calibration_test: OK\n");
    return g_fail ? 1 : 0;
}
