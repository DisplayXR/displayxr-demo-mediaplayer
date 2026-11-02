// SPDX-License-Identifier: Apache-2.0
//
// Per-panel display calibration (#133): which panel this is, and where its calibration
// LUT lives.
//
// The show grade (exposure -> CDL -> look LUT) is creative and follows the content. A
// calibration LUT is the opposite: it corrects THIS panel towards a target (Rec.709 /
// D65 / gamma 2.4, a site standard) and follows the unit, so it is keyed by the panel's
// identity and loaded automatically whenever the player runs on that panel.
//
// The identity is the monitor's EDID: manufacturer, product code and serial, the same
// <PNP>-<PROD>-<SERIAL> shape displayxr-cli uses for screen keys. XR_DXR_display_info
// only names the OS device (\\.\DISPLAY1 on Windows), which changes when the cable
// moves port, so on Windows the EDID is read from the device's registry key (no admin
// needed). Where that fails, and on macOS / Linux, the OS device name is the key.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mp::pro::cal {

// "<PNP>-<PROD>-<SERIAL>" from a 128-byte (or longer) EDID block: PNP = the three-letter
// manufacturer id, PROD = the product code as 4 hex digits, SERIAL = the 0xFF monitor
// serial descriptor if present, else the 32-bit serial number in decimal; with neither,
// "<PNP>-<PROD>". Empty when the block is not an EDID (bad header or too short).
std::string PanelKeyFromEdid(const uint8_t* edid, size_t size);

// A key safe to use as a file name: letters, digits, '-' and '_' kept, everything else '_'.
// Empty input gives "default".
std::string SanitizeKey(const std::string& key);

// The panel key for the OS display device the player is on (XrDisplayDesktopInfoDXR::
// deviceName). Windows: from the monitor's EDID, falling back to the sanitized device
// name. Elsewhere: the sanitized device name. "default" when the name is empty.
std::string PanelKeyForDevice(const std::string& deviceName);

// The folder calibration LUTs live in (created on demand by Install):
//   Windows: %APPDATA%\DisplayXR\MediaPlayerPro\calibration
//   macOS:   ~/Library/Application Support/DisplayXR/MediaPlayerPro/calibration
//   Linux:   $XDG_CONFIG_HOME (or ~/.config)/displayxr/mediaplayer-pro/calibration
// MEDIAPLAYER_PRO_CAL_DIR overrides it (tests, a shared site folder).
std::string CalibrationDir();

// <CalibrationDir()>/<key>.cube
std::string CalibrationPathFor(const std::string& key);

// Copy `cube` to CalibrationPathFor(key) so it loads on this panel from now on.
bool Install(const std::string& cube, const std::string& key, std::string& err);

// Delete the panel's installed calibration (no file = success).
bool Remove(const std::string& key, std::string& err);

}  // namespace mp::pro::cal
