// SPDX-License-Identifier: Apache-2.0
//
// CameraSelect (#90) — the PURE half of the live camera source: the tracking-camera
// denylist, the `--camera=<selector>` grammar, the default device pick, and the choice
// of capture format. No SDL and no platform headers, so it is unit-tested like
// StereoDetect (tests/camera_select_test.cpp). CameraSource fills these structs from
// SDL's camera API and calls in here for every decision.
//
// THE RULE this file exists to enforce: a 3D panel's own eye-tracking camera (e.g.
// "SpatialLabs Tracking Camera") enumerates as an ordinary camera. Opening it steals
// the stream from the eye tracker and the weave stops following the viewer. So a
// denied device is flagged at enumeration, never returned by a selector (not even by
// its exact name), and refused again by CameraSource::Open().
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mp {

struct CameraFormat {
    uint32_t fourccOrSdlFormat = 0;  // SDL_PixelFormat value (opaque here)
    int w = 0, h = 0;
    int fpsNum = 0, fpsDen = 1;
    bool mjpg = false;               // compressed: needs an app-side decoder
    bool nv12 = false;               // the zero-conversion format for FrameRing
};

struct CameraDevice {
    uint32_t id = 0;              // SDL_CameraID (0 = invalid)
    std::string name;             // SDL_GetCameraName
    bool denied = false;          // tracking/IR camera: listed in --camera-list as [blocked], NEVER opened
    std::vector<CameraFormat> formats;
};

// Case-insensitive substring denylist. Built-ins: "Tracking" (covers "SpatialLabs
// Tracking Camera"), "IR Camera", "Infrared", "Depth". Extended by `extraDenyEnv`, the
// value of MEDIAPLAYER_CAMERA_DENY ("a;b;c", empty entries ignored).
bool IsDeniedCameraName(const std::string& name, const std::string& extraDenyEnv);

// Selector grammar: "" / "auto" -> DefaultCamera; all digits -> index into the NON-denied
// list; anything else -> case-insensitive substring of the name (first match among the
// non-denied). Returns an index into `devs`, or -1. Never returns a denied device, EVEN
// IF the selector names it.
int SelectCamera(const std::vector<CameraDevice>& devs, const std::string& selector);

// Default pick: the first non-denied device whose name contains "SpatialLabs Eyes"; else
// the first non-denied device offering >= 3840 px width; else the first non-denied one.
int DefaultCamera(const std::vector<CameraDevice>& devs);

// The format to request. Size: exact (wantW, wantH) when offered, else the largest area.
// Rate: exactly wantFps when offered at that size, else the highest <= 60 (the lowest
// above 60 if nothing is <= 60). wantW/H/fps of 0 = "no preference". Among formats that
// tie on size and rate: NV12 > other uncompressed > MJPG. `allowMjpg=false` drops the
// compressed formats entirely (no in-app MJPG decoder yet, #90 follow-up).
bool ChooseFormat(const CameraDevice& d, int wantW, int wantH, int wantFps, CameraFormat& out,
                  bool allowMjpg = true);

// Frame rate of a format, rounded to the nearest integer (30000/1001 -> 30).
int FormatFps(const CameraFormat& f);

}  // namespace mp
