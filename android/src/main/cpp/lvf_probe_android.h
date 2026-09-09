// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// LvfProbeAndroid — container-level detection of a TWO-TRACK stereo mp4 ("LVF v2").
//
// The file is an ordinary .mp4 that carries one FULL view per video track rather than
// two views packed into one frame:
//
//   track L : H.264, mdhd language `abl`   (left eye)
//   track R : H.264, mdhd language `abr`   (right eye)   — same res, same fps, and
//                                                          every sample's PTS matches
//                                                          its left-eye twin exactly
//   track M : `mett` sample entry, MIME `application/vnd.leia.convergence+json`
//             (legacy: `application/convergence`). Each sample is
//             `{"convergence":<float>}` — a horizontal shift in FRACTION OF VIEW
//             WIDTH, half applied to each eye. POSITIVE = NEARER (content toward the
//             viewer), NEGATIVE = further behind the glass; a parallel-rig capture
//             therefore carries a negative value throughout. Sign derivation and its
//             evidence: sbs_renderer.cpp, drawAtlas().
//   track A : audio (ignored here; AudioPlayer opens its own extractor)
//
// This probe is the container layer of the layered stereo resolution (#45), and it sits
// ABOVE all of the existing layers: when it finds two eye tracks there is nothing left
// to guess, so the CPU cross-correlation probe (VideoStereoProbeAndroid) is skipped
// entirely. It opens its own short-lived AMediaExtractor, reads track formats and the
// (small) metadata track, and goes away — no codec is ever created.
//
// What it deliberately does NOT key on
// -----------------------------------
// `tkhd` enabled/flags and `alternate_group`. A spec-shaped LVF marks the right track
// tkhd-disabled so a dumb player shows one eye — but a REAL vendor-camera v1 recording has
// BOTH video tracks tkhd-enabled (flags 0x7, alternate_group 0, Android MPEG4Writer's
// defaults), so a tkhd-driven rule sees two "primary" tracks and picks wrong or picks
// neither. The LANGUAGE tag is the signal here; tkhd-enabled would only ever be a
// tiebreak for a file with no language tags at all, and this reader does not even need
// that (it falls back to track order, which the vendor player SDK does too).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mp {

struct LvfProbeAndroid {
	struct ConvSample {
		int64_t ptsUs = 0;
		float value = 0.0f;
	};

	struct Result {
		// True only when TWO eye tracks were identified. Everything else in here is
		// diagnostic; the caller must not act on leftTrack/rightTrack unless this is set.
		bool ok = false;
		int leftTrack = -1;
		int rightTrack = -1;
		int width = 0, height = 0;  // from the LEFT track's format
		float fps = 0.0f;           // 0 when the container does not say
		int64_t durationUs = 0;
		// FIRST sample time of the left video track and of the convergence track, as
		// the extractor itself reports them (AMediaExtractor_getSampleTime), -1 if
		// none. These exist to make ONE question observable without a rebuild: a v1
		// vendor-camera file's video traks carry an initial EMPTY edit (elst 167.8 ms)
		// while its convergence trak does not, so whether this device's MPEG4Extractor
		// applies that edit decides whether video PTS start at 167800 or at 0 -- and
		// therefore whether the two timelines line up. Nothing here does elst
		// arithmetic of its own: convergence is interpolated purely in the reported
		// timeline, and these two numbers are how you tell whether that was enough.
		int64_t firstVideoPtsUs = -1;
		int64_t firstConvPtsUs = -1;
		// Eye-track identification: "language" (abl/abr, authoritative) or "order"
		// (untagged fallback: exactly two matching video tracks, first = left).
		const char *how = "";
		int videoTracks = 0;  // video tracks that looked like a real eye track
		int coverTracks = 0;  // video tracks skipped as attached_pic / cover art

		int convTrack = -1;
		// OWNED, not a borrowed pointer: AMediaFormat_getString returns a pointer into
		// the AMediaFormat, and every format this probe opens is deleted before the
		// Result reaches the caller. A const char* here read freed memory.
		std::string convMime;
		std::vector<ConvSample> convergence;
		// The extractor listed a track whose mime it could not name at all. On some
		// devices the NDK MPEG4 extractor hides `mett` tracks; when that happens we
		// cannot read convergence and must fall back to a constant 0. Distinguishes
		// "the file has no convergence" from "we could not see it".
		int unnamedTracks = 0;
	};

	// The fd is dup()ed internally (the extractor does that itself and reads with
	// absolute offsets), so the caller keeps ownership and its file position.
	static Result RunFd(int fd, int64_t offset, int64_t length);
	static Result RunPath(const char *path);

	// Convergence at `ptsUs`: linear interpolation between the bracketing samples,
	// clamped to the first/last value outside the sampled range, 0 when there are none.
	// `samples` must be sorted by ptsUs (RunFd/RunPath guarantee it).
	static float ConvergenceAt(const std::vector<ConvSample> &samples, int64_t ptsUs);

	// Filename opt-out: a name containing `_noreconv` means "do not apply convergence";
	// `_reconv` (and anything else) means apply it. Case-insensitive.
	static bool ApplyConvergenceForName(const std::string &name);
};

}  // namespace mp
