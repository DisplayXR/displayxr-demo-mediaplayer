// SPDX-License-Identifier: Apache-2.0
//
// dxrmedia — video side of the format (FORMAT.md 3): the 32-byte timed-metadata
// sample, and MP4 post-passes on a finished file (XMP uuid box, st3d box).
// The muxing itself is the app's (MediaMuxer); demuxing is the consumers'
// (MediaExtractor / FFmpeg). The readers here are for tests and debugging.
#pragma once

#include "dxrmedia/metadata.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxrmedia {

constexpr const char* kMetadataMime = "application/x-displayxr-stereo";
constexpr size_t kFrameSampleSize = 32;

enum class ConvergenceSource : uint8_t { Auto = 0, Tap = 1, Manual = 2 };

// FORMAT.md 3.3 flags word.
enum FrameFlags : uint16_t {
	kFlagConvergenceValid = 1u << 0,          // convergence (offset 4)
	kFlagFocusValid = 1u << 1,                // focusDistanceMm (offset 20)
	kFlagSceneCut = 1u << 2,                  // a scene cut at this frame
	kFlagDisparityRangeValid = 1u << 3,       // disparityNear + disparityFar (12, 16)
	kFlagExposureValid = 1u << 4,             // exposureMs + iso (24, 28)
	kFlagConvergenceDistanceValid = 1u << 5,  // convergenceDistanceMm (8)
	kFlagsKnown = 0x3F,
};

// FORMAT.md 3.3. Set a *Valid bool only when the value is real: the encoder writes 0
// in every field whose bit is clear, and the decoder zeroes such fields so a reader
// cannot use them by accident.
struct FrameMeta {
	uint8_t version = 1;
	ConvergenceSource convergenceSource = ConvergenceSource::Auto;
	bool convergenceValid = false;          // bit 0
	bool focusValid = false;                // bit 1
	bool sceneCut = false;                  // bit 2
	bool disparityRangeValid = false;       // bit 3
	bool exposureValid = false;             // bit 4 (exposure and ISO)
	bool convergenceDistanceValid = false;  // bit 5
	// Decode: the raw flags word, including bits this version does not know.
	// Encode: ignored; the word is built from the bools above (unknown bits are 0).
	uint16_t flags = 0;
	float convergence = 0;
	float convergenceDistanceMm = 0;
	float disparityNear = 0;
	float disparityFar = 0;
	float focusDistanceMm = 0;
	float exposureMs = 0;
	uint16_t iso = 0;
	int16_t rightMinusLeftUs = 0;           // no validity bit
};

// Exactly 32 bytes, little-endian, version field forced to 1.
std::array<uint8_t, kFrameSampleSize> encode_frame_meta(const FrameMeta& m);
// Accepts samples of >= 32 bytes with version >= 1 (later versions append). Fields
// whose validity bit is clear come back 0.
bool decode_frame_meta(const uint8_t* data, size_t size, FrameMeta& out);
// Saturating conversion for the i16 field: (right - left) ns -> us.
int16_t saturate_delta_us(int64_t rightMinusLeftNs);

// XMP UUID BE7ACFCB-97A9-42E8-9C71-999491E3AFAC.
extern const uint8_t kXmpUuid[16];

// FORMAT.md 3.2: largest XMP packet the video post-pass writes.
constexpr size_t kMaxVideoXmpPacket = 1u << 20;  // 1 MiB

// ---------------------------------------------------------------- MP4 post-pass
//
// Every MP4 entry point exists in two forms: on an open file descriptor (`*_fd`,
// what an Android app uses: it opens the file it owns in app-private storage,
// finalises it, then publishes it to MediaStore) and on a path (a thin wrapper that
// opens the file and calls the fd form). The fd must be open read-write for the
// modifying calls and stays open; its file offset is not used.
//
// Guarantees (see README "Crash safety"):
//  - Everything is validated before the first byte is written; a failed validation
//    leaves the file untouched.
//  - Media data never moves and no temporary file is ever created. Edits are
//    appends at the end of the file plus single in-place header writes of at most
//    8 bytes, ordered (with fsync between steps) so the file is a playable MP4 after
//    every step: a crash or write error at any point leaves either the original
//    file, the finished one, or a valid intermediate (the old metadata in charge,
//    plus unused `free`/`skip` boxes).
//  - Non-fragmented files only; size-0 ("to end of file") last boxes are given an
//    explicit size first; up to 7 trailing bytes after the last box are tolerated
//    (and overwritten by the append).

struct Mp4FinalizeOptions {
	bool addSt3d = true;              // FORMAT.md 3.2 SHOULD: st3d left-right on every video sample entry
	const StereoMeta* xmp = nullptr;  // when set: write the XMP uuid box (layout forced to sbs-lr)
};

struct Mp4FinalizeResult {
	int st3dAdded = 0;          // sample entries that got st3d (0: already tagged)
	bool xmpWritten = false;
	// Where moov ended up. A moov that was before mdat stays there when the free
	// space after it can take the growth (13 bytes per st3d; a following free box of
	// 13..20 bytes is absorbed whole); otherwise moov moves to the end of the file
	// (no data is copied, but the file is no longer "fast start").
	bool moovAtFront = false;
	bool moovMovedToEnd = false;
};

// THE finalisation call (after MediaMuxer.stop()): st3d and XMP together, in one
// pass, in the right order. Prefer this over the two low-level calls below.
bool mp4_finalize_fd(int fd, const Mp4FinalizeOptions& opt, Mp4FinalizeResult* result = nullptr,
                     std::string* error = nullptr);
bool mp4_finalize(const std::string& path, const Mp4FinalizeOptions& opt, Mp4FinalizeResult* result = nullptr,
                  std::string* error = nullptr);

// Low-level pieces of mp4_finalize. Either order works and each is safe on its own,
// but calling them separately can cost an extra relocation of moov: if you need both,
// call mp4_finalize. Order when used separately: st3d first, then XMP.
// st3d: `*added` = sample entries tagged (0 when already tagged; not an error).
bool mp4_add_st3d_fd(int fd, std::string* error = nullptr, int* added = nullptr);
bool mp4_add_st3d(const std::string& path, std::string* error = nullptr, int* added = nullptr);
// XMP: appends the packet as the last box; an older XMP box becomes `free` only after
// the new one is safely written.
bool mp4_write_xmp_fd(int fd, const StereoMeta& meta, std::string* error = nullptr);
bool mp4_write_xmp(const std::string& path, const StereoMeta& meta, std::string* error = nullptr);
bool mp4_write_xmp_packet_fd(int fd, const std::string& packet, std::string* error = nullptr);
bool mp4_write_xmp_packet(const std::string& path, const std::string& packet, std::string* error = nullptr);

struct Mp4Track {
	uint32_t trackId = 0;
	std::string handler;       // 'vide', 'soun', 'meta', ...
	std::string sampleEntry;   // 'hvc1', 'mp4a', 'mett', ...
	std::string mime;          // mett mime_format, if any
	bool hasSt3d = false;
	int st3dMode = -1;
	uint32_t sampleCount = 0;
};

struct Mp4Info {
	std::vector<std::string> topLevel;  // box types in file order
	std::vector<Mp4Track> tracks;
	bool hasXmp = false;
	std::string xmpPacket;              // the first XMP uuid box
	int metadataTrack = -1;             // index into tracks of the dxr metadata track
	uint64_t trailingBytes = 0;         // ignored bytes after the last box (< 8)
};

// Walks the box tree (bounded). False on a structurally broken file. Read-only.
bool mp4_probe_fd(int fd, Mp4Info& out, std::string* error = nullptr);
bool mp4_probe(const std::string& path, Mp4Info& out, std::string* error = nullptr);

// Raw samples of track `trackId` via stsz/stsc/stco/co64 (at most `maxSamples`).
// Rejects malformed sample tables (stsc runs not starting at chunk 1, not strictly
// increasing, or with 0 samples per chunk). Work is linear in the table sizes.
bool mp4_read_samples_fd(int fd, uint32_t trackId, size_t maxSamples, std::vector<std::vector<uint8_t>>& samples,
                         std::string* error = nullptr);
bool mp4_read_samples(const std::string& path, uint32_t trackId, size_t maxSamples,
                      std::vector<std::vector<uint8_t>>& samples, std::string* error = nullptr);

}  // namespace dxrmedia
