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
// The identity is the runtime's stable per-display key (XrDisplayStableKeyDXR,
// XR_DXR_display_info v24): the key `displayxr-cli displays --claims` prints, built from
// the panel's EDID and, where the EDID has no serial, the serial the vendor plug-in
// reports. It survives a reboot and a desktop rearrangement. The player looks up the
// display its window is on (XrSession::EnumerateDisplays) and asks again when the
// displays change or the window moves. A runtime older than v24 has no key: the OS
// device name (\\.\DISPLAY1, a RandR output) stands in, which follows the port.
#pragma once

#include <string>

namespace mp::pro::cal {

// A key safe to use as a file name: letters, digits, '-' and '_' kept, everything else '_',
// with the leading run of '_' a device prefix leaves dropped. Empty input gives "default".
std::string SanitizeKey(const std::string& key);

// The key to store a panel's calibration under: the runtime's stable key as it is (it is
// file-safe by contract) when it has one, else the sanitized OS device name, else "default".
std::string PanelKey(const std::string& runtimeKey, const std::string& deviceName);

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

// Delete the panel's installed calibration (no file = success). A delete that fails
// returns false with `err` set; the file is then still there and loads next run.
bool Remove(const std::string& key, std::string& err);

// Whether `path` is the panel's kept file (CalibrationPathFor(key)), i.e. not a run-only
// calibration (MEDIAPLAYER_PRO_CAL, or one whose keep failed). Empty path = false. Used by
// Remove in the Inspector (#169): removing a run-only calibration drops it for this run and
// must not delete the file kept for the panel.
bool IsKeptFile(const std::string& path, const std::string& key);

}  // namespace mp::pro::cal
