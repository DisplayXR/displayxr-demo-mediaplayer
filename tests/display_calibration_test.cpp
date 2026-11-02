// SPDX-License-Identifier: Apache-2.0
// Per-panel display calibration (#133): the panel key from an EDID, and the file it maps to.
#include "pro/DisplayCalibration.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

static int g_fail = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                  \
        }                                                              \
    } while (0)

using namespace mp::pro::cal;

// A minimal 128-byte EDID: header, manufacturer, product, serial number, and an
// optional 0xFF serial-string descriptor in the first descriptor slot.
static void MakeEdid(uint8_t* e, const char pnp[3], unsigned prod, uint32_t serial, const char* serialText) {
    std::memset(e, 0, 128);
    const uint8_t h[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::memcpy(e, h, 8);
    const unsigned m = ((unsigned)(pnp[0] - 'A' + 1) << 10) | ((unsigned)(pnp[1] - 'A' + 1) << 5) |
                       (unsigned)(pnp[2] - 'A' + 1);
    e[8] = (uint8_t)(m >> 8);
    e[9] = (uint8_t)m;
    e[10] = (uint8_t)prod;
    e[11] = (uint8_t)(prod >> 8);
    e[12] = (uint8_t)serial;
    e[13] = (uint8_t)(serial >> 8);
    e[14] = (uint8_t)(serial >> 16);
    e[15] = (uint8_t)(serial >> 24);
    if (serialText) {
        uint8_t* d = e + 54;
        d[3] = 0xFF;
        size_t n = std::strlen(serialText);
        for (size_t i = 0; i < 13; ++i) d[5 + i] = i < n ? (uint8_t)serialText[i] : (i == n ? 0x0A : 0x20);
    }
}

int main() {
    uint8_t e[128];

    // The serial string wins over the numeric serial; trailing padding is trimmed.
    MakeEdid(e, "LEI", 0x1234, 77, "QI012150D10");
    CHECK(PanelKeyFromEdid(e, 128) == "LEI-1234-QI012150D10");

    // No serial string: the 32-bit serial in decimal.
    MakeEdid(e, "ACR", 0x0000, 4353, nullptr);
    CHECK(PanelKeyFromEdid(e, 128) == "ACR-0000-4353");

    // Neither: maker + product only. Two panels of that model then share a key.
    MakeEdid(e, "DEL", 0xA0B1, 0, nullptr);
    CHECK(PanelKeyFromEdid(e, 128) == "DEL-A0B1");

    // Not an EDID: bad header, or too short.
    MakeEdid(e, "DEL", 1, 1, nullptr);
    e[0] = 0x12;
    CHECK(PanelKeyFromEdid(e, 128).empty());
    MakeEdid(e, "DEL", 1, 1, nullptr);
    CHECK(PanelKeyFromEdid(e, 127).empty());
    CHECK(PanelKeyFromEdid(nullptr, 128).empty());

    // A serial string with characters a file name cannot carry.
    MakeEdid(e, "SAM", 0x7001, 0, "AB/12:CD");
    CHECK(PanelKeyFromEdid(e, 128) == "SAM-7001-AB_12_CD");

    // Device-name fallback keys (macOS / Linux, or no EDID).
    CHECK(SanitizeKey("\\\\.\\DISPLAY1") == "DISPLAY1");
    CHECK(SanitizeKey("HDMI-1") == "HDMI-1");
    CHECK(SanitizeKey("") == "default");
    CHECK(SanitizeKey("///") == "default");
    CHECK(PanelKeyForDevice("") == "default");

    // The calibration folder honours MEDIAPLAYER_PRO_CAL_DIR; Install / Remove round-trip.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dxr_cal_test";
    std::filesystem::remove_all(dir);
#if defined(_WIN32)
    _putenv_s("MEDIAPLAYER_PRO_CAL_DIR", dir.string().c_str());
#else
    setenv("MEDIAPLAYER_PRO_CAL_DIR", dir.string().c_str(), 1);
#endif
    CHECK(CalibrationDir() == dir.string());
    CHECK(CalibrationPathFor("LEI-1234-X") == (dir / "LEI-1234-X.cube").string());
    const std::filesystem::path src = std::filesystem::temp_directory_path() / "dxr_cal_src.cube";
    { std::ofstream(src) << "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"; }
    std::string err;
    CHECK(Install(src.string(), "LEI-1234-X", err));
    CHECK(std::filesystem::exists(dir / "LEI-1234-X.cube"));
    CHECK(Install((dir / "LEI-1234-X.cube").string(), "LEI-1234-X", err));  // installing the installed file
    CHECK(Remove("LEI-1234-X", err));
    CHECK(!std::filesystem::exists(dir / "LEI-1234-X.cube"));
    CHECK(Remove("LEI-1234-X", err));  // nothing to remove is fine
    std::filesystem::remove_all(dir);
    std::filesystem::remove(src);

    if (g_fail) std::fprintf(stderr, "display_calibration_test: %d failure(s)\n", g_fail);
    else std::printf("display_calibration_test: OK\n");
    return g_fail ? 1 : 0;
}
