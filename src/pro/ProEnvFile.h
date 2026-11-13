// SPDX-License-Identifier: Apache-2.0
//
// ProEnvFile - Media Player Pro's per-machine settings file, pro.env: KEY=VALUE lines put into
// the process environment at start, before anything reads them (the capture levers, the
// 10-bit atlas, and the runtime's own DXR_* knobs, which the runtime reads when it loads at
// the first OpenXR call). A variable already set in the environment wins over the file.
//
// Why a file: the station's settings were environment variables and Start-menu shortcut
// arguments, and the installer rewrites the shortcut on every upgrade. The file is the user's
// and survives a reinstall (docs/CAPTURE-LEVERS.md).
//
// Where: MEDIAPLAYER_PRO_ENV_FILE if set (empty or "off" = no file), else
//   Windows  %APPDATA%\DisplayXR\MediaPlayerPro\pro.env
//   macOS    ~/Library/Application Support/DisplayXR/MediaPlayerPro/pro.env
//   Linux    $XDG_CONFIG_HOME (or ~/.config)/displayxr/mediaplayer-pro/pro.env
#pragma once

#include <string>

namespace mp::pro {

// The file's path ("" when there is none to read: switched off, or no home folder).
std::string ProEnvFilePath();
// Reads and applies the file, logging one line per variable it set or left (call once, first
// thing in main). A missing file is not an error.
void LoadProEnvFile();

}  // namespace mp::pro
