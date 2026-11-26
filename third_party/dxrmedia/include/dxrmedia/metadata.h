// SPDX-License-Identifier: Apache-2.0
//
// dxrmedia — metadata model of the DisplayXR stereo media format, version 1
// (docs/FORMAT.md). Plain data; no I/O here.
//
// Absent values: std::optional is empty, or a std::string is empty. FORMAT.md
// section 0: "Unknown = attribute absent (never 0, never NaN)". A writer drops a
// non-finite number silently (treats it as absent).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dxrmedia {

constexpr int kFormatVersion = 1;

// XMP namespace and the standard XMP APP1 signature (FORMAT.md 1.3).
constexpr const char* kDxrNamespace = "http://ns.displayxr.org/stereo/1.0/";
constexpr const char* kDxrPrefix = "dxr";

// FORMAT.md 1.2. Pixel dimensions are the STORED image (one eye for the MPF still,
// both eyes for the SBS export); the writer always sets them from the JPEG frame
// header. One eye's size is StereoMeta::eyeWidth/eyeHeight.
struct ExifMeta {
	std::string make;                       // Build.MANUFACTURER
	std::string model;                      // Build.MODEL
	std::string software;                   // "DisplayXR Camera <version>"
	std::string dateTimeOriginal;           // "YYYY:MM:DD HH:MM:SS" (EXIF form)
	std::string offsetTimeOriginal;         // "+HH:MM" / "-HH:MM"
	std::optional<double> exposureTimeS;    // seconds
	std::optional<uint32_t> iso;            // ISOSpeedRatings (SHORT, clamped to 65535)
	std::optional<double> focalLengthMm;
	std::optional<double> fNumber;
	std::optional<double> subjectDistanceM; // metres; only when the lens reports focus
	std::optional<uint32_t> pixelXDimension;
	std::optional<uint32_t> pixelYDimension;
	// "DisplayXR stereo v1; <facing>". Empty on write = generated from StereoMeta::facing
	// (or left out when facing is empty too).
	std::string userComment;
	// Read only: the Orientation tag found in the file (the writer always writes 1).
	std::optional<uint16_t> orientation;
};

// FORMAT.md 1.5. Intrinsics in pixels at the stated size.
struct LensIntrinsics {
	double fx = 0, fy = 0, cx = 0, cy = 0;
	std::string model = "none";             // none | radtan5 | radtan8 | kb4
	std::vector<double> coeffs;
};

struct CalibrationRaw {
	std::array<int, 2> size{{0, 0}};
	LensIntrinsics left, right;
	std::array<double, 9> rRightFromLeft{{1, 0, 0, 0, 1, 0, 0, 0, 1}};  // row-major
	std::array<double, 3> tRightFromLeftMm{{0, 0, 0}};
};

struct CalibrationRectified {
	std::array<int, 2> size{{0, 0}};
	double fx = 0, fy = 0, cxLeft = 0, cxRight = 0, cy = 0;
	std::array<double, 9> rLeft{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
	std::array<double, 9> rRight{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
	// FORMAT.md 1.5 (additive): the vertical row refinement applied after the
	// rotations, in normalised rectified units: D(y) = verticalOffset + verticalSlope * y
	// (right minus left, +y down), split symmetrically between the eyes. Absent = none.
	std::optional<double> verticalOffset, verticalSlope;
};

struct Calibration {
	std::optional<CalibrationRaw> raw;
	std::optional<CalibrationRectified> rectified;
	std::string source;                     // measured | factory | unknown
	std::string sourceId;                   // free text
};

// The dxr: XMP attributes (FORMAT.md 1.3). Same struct for stills and video.
struct StereoMeta {
	std::optional<int> version;             // dxr:Version; the writer always writes kFormatVersion
	std::string layout;                     // mpf-pair | sbs-lr (writer fills it per entry point)
	std::optional<int> eyeWidth, eyeHeight;
	std::optional<bool> rectified;
	std::string alignment;                  // parallel
	std::optional<double> convergence;      // d = x_left - x_right, fraction of one eye's width
	std::optional<double> convergenceDistanceMm;
	std::string convergenceSource;          // auto | tap | manual
	std::optional<double> disparityNear, disparityFar;
	std::optional<double> baselineMm;
	std::optional<double> focalFrac, focalYFrac, cxFrac, cyFrac;
	std::string facing;                     // front | rear | external
	std::optional<double> focusDistanceMm;
	std::optional<int64_t> captureTimeLeftNs, captureTimeRightNs;
	std::string calibrationSource;          // measured | factory | unknown | none
	std::string cameraName;                 // dxr:CameraName: the source's own label (from the runtime)
	std::string pixelSource;                // dxr:PixelSource: still | stream
	std::optional<bool> sourceLossy;        // dxr:SourceLossy: the pixels went through lossy compression before rectification
	// dxr:Calibration. On write: `calibration` is serialised if set, else
	// `calibrationJson` is written verbatim if non-empty. On read: both are filled
	// (`calibration` only when the JSON parses).
	std::optional<Calibration> calibration;
	std::string calibrationJson;
	std::optional<int> disparityMapIndex;   // the stills writer sets these two itself
	std::optional<double> disparityMapNear, disparityMapFar;
	std::string disparityMapEye;            // left
	std::optional<int> rawIndex;
};

struct StillMeta {
	ExifMeta exif;
	StereoMeta stereo;
};

}  // namespace dxrmedia
