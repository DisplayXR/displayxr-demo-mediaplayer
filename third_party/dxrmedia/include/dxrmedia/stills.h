// SPDX-License-Identifier: Apache-2.0
//
// dxrmedia — stills writer and reader (FORMAT.md 1 and 2).
//
// The library never encodes or decodes pixels: it takes JPEG byte buffers from the
// caller's encoder and rewrites their header segments.
#pragma once

#include "dxrmedia/metadata.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxrmedia {

// A non-owning view of bytes. It borrows: the memory must outlive every use of the
// view (for StillInputs: until write_still returns). A view of a temporary vector
// cannot be made: `in.left = encode(...)` with a vector-returning encode() does not
// compile; keep the vector in a named variable.
struct Bytes {
	const uint8_t* data = nullptr;
	size_t size = 0;
	Bytes() = default;
	Bytes(const uint8_t* d, size_t n) : data(d), size(n) {}
	Bytes(const std::vector<uint8_t>& v) : data(v.data()), size(v.size()) {}  // NOLINT: lvalues only
	Bytes(std::vector<uint8_t>&&) = delete;        // would dangle
	Bytes(const std::vector<uint8_t>&&) = delete;  // would dangle
	bool empty() const { return size == 0; }
};

// What the writer had to leave out of the caller's JPEGs (FORMAT.md 1.1).
struct StillWriteReport {
	bool droppedEncoderMpf = false;    // an input carried its own MPF index (e.g. Ultra HDR)
	bool droppedTrailingData = false;  // non-zero bytes after an input's EOI (e.g. an Ultra HDR gain map)
	std::vector<std::string> warnings; // one line per drop, naming the input
};

// All views borrow (see Bytes); the buffers must stay alive until write_still returns.
struct StillInputs {
	Bytes left;            // required
	Bytes right;           // required
	Bytes disparityMap;    // optional, 8-bit greyscale JPEG (FORMAT.md 1.4)
	Bytes rawLeft;         // optional; rawLeft and rawRight come together or not at all
	Bytes rawRight;
};

// FORMAT.md 1: build the single-file .jpg. Each input must be a complete JPEG
// (SOI ... EOI). The writer strips the inputs' own APP0 JFIF, APP1 Exif/XMP and
// APP2 MPF segments (other segments, e.g. an ICC profile, are kept), then writes
//   image 0 (left):  SOI, APP1 Exif, APP1 XMP, APP2 MPF (index IFD + attribute IFD), rest
//   image 1 (right): SOI, APP2 MPF (attribute IFD, image number 2), rest
//   image 2.. :      SOI, APP2 MPF (attribute IFD), rest
// It overrides meta.stereo.version (1), layout ("mpf-pair"), disparityMapIndex and
// rawIndex from what is actually stored, and EXIF Orientation (1).
// Returns false with `error` set on bad input (missing view, not a JPEG, XMP over
// 64 KB, file over 4 GB).
// Ultra HDR inputs lose their gain map and their own MPF (reported in `report`).
bool write_still(const StillInputs& in, const StillMeta& meta, std::vector<uint8_t>& out,
                 std::string* error = nullptr, StillWriteReport* report = nullptr);

// FORMAT.md 2: the side-by-side export. `sbs` is one JPEG (left | right); EXIF and
// XMP (dxr:Layout="sbs-lr") are injected; no MPF. EXIF PixelX/YDimension become the
// stored (full side-by-side) size; meta.stereo.eyeWidth/Height are one eye (filled
// with half the width when not given).
bool write_sbs_still(Bytes sbs, const StillMeta& meta, std::vector<uint8_t>& out,
                     std::string* error = nullptr, StillWriteReport* report = nullptr);
// Same, for a buffer held in a vector (a temporary is fine: nothing is kept).
inline bool write_sbs_still(const std::vector<uint8_t>& sbs, const StillMeta& meta, std::vector<uint8_t>& out,
                            std::string* error = nullptr, StillWriteReport* report = nullptr) {
	return write_sbs_still(Bytes(sbs.data(), sbs.size()), meta, out, error, report);
}

enum class ReadStatus {
	Ours,       // a JPEG with an XMP dxr:Version (FORMAT.md 4)
	NotOurs,    // a well-formed JPEG / MPO without dxr:Version (third-party file)
	Invalid,    // not a JPEG, truncated, or a damaged MPF index
};

struct ImageSpan {
	uint64_t offset = 0;      // file offset of the image's SOI
	uint64_t size = 0;        // bytes, SOI through EOI
	uint32_t attribute = 0;   // MPF MP Entry attribute (0 when the file has no MPF)
};

struct StillFile {
	ReadStatus status = ReadStatus::Invalid;
	std::string error;                 // why Invalid
	std::vector<std::string> warnings; // tolerated oddities (third-party files)
	bool hasExif = false, hasXmp = false, hasMpf = false;
	StillMeta meta;
	std::string xmpPacket;             // raw packet as found
	// MPF index order; without MPF, one span covering the primary JPEG.
	std::vector<ImageSpan> images;

	// Convenience lookups (nullptr when absent). left/right = MPF index 0/1.
	const ImageSpan* left() const { return images.size() > 0 ? &images[0] : nullptr; }
	const ImageSpan* right() const;          // nullptr for an SBS or a mono JPEG
	const ImageSpan* disparityMap() const;   // via dxr:DisparityMapIndex
	const ImageSpan* rawLeft() const;        // via dxr:RawIndex
	const ImageSpan* rawRight() const;
};

// Parse a file held in memory. Never reads out of bounds; any structural failure
// gives status Invalid with `error` set.
// The result holds offsets only, never pointers into `file`.
StillFile read_still(Bytes file);
inline StillFile read_still(const std::vector<uint8_t>& file) { return read_still(Bytes(file.data(), file.size())); }
StillFile read_still_file(const std::string& path);

const char* to_string(ReadStatus s);

}  // namespace dxrmedia
