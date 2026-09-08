// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0

#include "lvf_probe_android.h"

#include <android/log.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace mp {
namespace {

// The convergence metadata track's sample-entry MIME. The spec name is the +json one;
// LeiaCam2 v1 recordings in the field carry the short legacy name (verified on a real
// capture: `mett` sample entry, mime_format "application/convergence"), so both are
// accepted and anything else containing "convergence" is taken too.
constexpr const char *kConvMimeSpec = "application/vnd.leia.convergence+json";
constexpr const char *kConvMimeLegacy = "application/convergence";

// A convergence sample is a handful of bytes of JSON; nothing sane is bigger.
constexpr size_t kMetaSampleCap = 4096;
// Never let a malformed/huge metadata track hold the open path.
constexpr int kMaxConvSamples = 20000;

bool
mimeIsVideo(const char *m)
{
	return m != nullptr && std::strncmp(m, "video/", 6) == 0;
}

// A cover-art / attached_pic track. ffmpeg writes a LeiaCam2 v1 file's thumbnail as a
// one-sample mjpeg "video" track, and counting it as an eye track turns a legitimate
// two-eye file into a "three video tracks, give up" file. The mime test catches the
// common shapes; the sample count is the one that is actually definitive, so the caller
// pairs this with countSamplesUpTo().
bool
mimeIsCoverArt(const char *m)
{
	if (m == nullptr) return false;
	if (std::strncmp(m, "image/", 6) == 0) return true;
	return std::strstr(m, "mjpeg") != nullptr || std::strstr(m, "jpeg") != nullptr ||
	       std::strstr(m, "png") != nullptr;
}

// First sample time of one track, as the EXTRACTOR reports it -- i.e. after whatever
// edit-list handling this device's MPEG4Extractor does or does not do. -1 if the track
// yields no sample. Leaves no track selected.
int64_t
firstSampleTimeUs(AMediaExtractor *ex, int track)
{
	if (AMediaExtractor_selectTrack(ex, (size_t)track) != AMEDIA_OK) return -1;
	AMediaExtractor_seekTo(ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
	const int64_t t = AMediaExtractor_getSampleTime(ex);
	AMediaExtractor_unselectTrack(ex, (size_t)track);
	return t;
}

// Count this track's samples, stopping at `limit`. Cheap: getSampleTime/advance walk the
// sample tables, no sample data is read. Leaves no track selected.
int
countSamplesUpTo(AMediaExtractor *ex, int track, int limit)
{
	if (AMediaExtractor_selectTrack(ex, (size_t)track) != AMEDIA_OK) return 0;
	AMediaExtractor_seekTo(ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
	int n = 0;
	while (n < limit && AMediaExtractor_getSampleTime(ex) >= 0) {
		++n;
		if (!AMediaExtractor_advance(ex)) break;
	}
	AMediaExtractor_unselectTrack(ex, (size_t)track);
	return n;
}

// `{"convergence":-0.0507}` — one number out of a tiny fixed-shape object. A hand parse
// keeps this TU free of nlohmann and of exceptions on the open path; anything that does
// not match returns false and the sample is skipped rather than poisoning the curve.
bool
parseConvergence(const char *json, size_t len, float *out)
{
	// The buffer is not NUL-terminated; give strstr/strtof a bounded copy.
	char buf[kMetaSampleCap + 1];
	const size_t n = len < kMetaSampleCap ? len : kMetaSampleCap;
	std::memcpy(buf, json, n);
	buf[n] = '\0';
	const char *k = std::strstr(buf, "convergence");
	if (k == nullptr) return false;
	const char *c = std::strchr(k, ':');
	if (c == nullptr) return false;
	++c;
	while (*c == ' ' || *c == '\t' || *c == '"') ++c;
	char *end = nullptr;
	const float v = std::strtof(c, &end);
	if (end == c) return false;
	*out = v;
	return true;
}

LvfProbeAndroid::Result
run(AMediaExtractor *ex)
{
	LvfProbeAndroid::Result out;

	struct TrackInfo {
		int index = -1;
		const char *mime = "";  // owned by its AMediaFormat, kept alive below
		char lang[8] = {};
		int32_t w = 0, h = 0;
		float fps = 0.0f;
		int64_t durationUs = 0;
		bool cover = false;
	};
	std::vector<TrackInfo> vids;
	std::vector<AMediaFormat *> keep;  // formats whose `mime` pointers we still hold

	const size_t nTracks = AMediaExtractor_getTrackCount(ex);
	int convTrack = -1;
	std::string convMime;
	for (size_t i = 0; i < nTracks; ++i) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
		if (f == nullptr) {
			out.unnamedTracks++;
			continue;
		}
		const char *m = nullptr;
		if (!AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) || m == nullptr) {
			// The extractor listed the track but will not name it. On a device whose
			// MPEG4 extractor hides `mett` tracks this is what a convergence track
			// looks like from here, so record it: it is the difference between "no
			// convergence in the file" and "we cannot see the convergence".
			out.unnamedTracks++;
			AMediaFormat_delete(f);
			continue;
		}
		if (mimeIsVideo(m)) {
			TrackInfo t;
			t.index = (int)i;
			t.mime = m;
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &t.w);
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &t.h);
			int32_t fr = 0;
			if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, &fr)) t.fps = (float)fr;
			int64_t dur = 0;
			if (AMediaFormat_getInt64(f, AMEDIAFORMAT_KEY_DURATION, &dur)) t.durationUs = dur;
			const char *lang = nullptr;
			if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_LANGUAGE, &lang) && lang != nullptr) {
				std::strncpy(t.lang, lang, sizeof(t.lang) - 1);
			}
			t.cover = mimeIsCoverArt(m);
			vids.push_back(t);
			keep.push_back(f);
			continue;
		}
		if (convTrack < 0 && (std::strcmp(m, kConvMimeSpec) == 0 ||
		                      std::strcmp(m, kConvMimeLegacy) == 0 ||
		                      std::strstr(m, "convergence") != nullptr)) {
			convTrack = (int)i;
			convMime = m;  // copied: `f` is deleted before this Result is returned
			AMediaFormat_delete(f);
			continue;
		}
		AMediaFormat_delete(f);
	}

	// ── Eye tracks ────────────────────────────────────────────────────────────
	// LANGUAGE FIRST, always. `tkhd` enabled/flags and alternate_group are NOT
	// consulted: Android's MPEG4Writer stamps every track flags=0x7 and
	// alternate_group=1, so on a real LeiaCam2 recording both eyes read as "enabled"
	// and same-group, and a tkhd-driven rule picks the wrong track or none.
	int li = -1, ri = -1;
	for (size_t k = 0; k < vids.size(); ++k) {
		if (std::strncmp(vids[k].lang, "abl", 3) == 0 && li < 0) li = (int)k;
		if (std::strncmp(vids[k].lang, "abr", 3) == 0 && ri < 0) ri = (int)k;
	}
	if (li >= 0 && ri >= 0) {
		out.how = "language";
	} else {
		// No tags. Fall back to track ORDER, but only for a file that looks exactly
		// like a two-eye file: two non-cover video tracks with identical dimensions.
		// (Cover art is confirmed by sample count, not by mime alone — a one-sample
		// track is a thumbnail no matter what it calls itself.)
		std::vector<int> eyes;
		for (size_t k = 0; k < vids.size(); ++k) {
			const bool oneSample = countSamplesUpTo(ex, vids[k].index, 2) <= 1;
			if (vids[k].cover || oneSample) {
				out.coverTracks++;
				continue;
			}
			eyes.push_back((int)k);
		}
		if (eyes.size() == 2 && vids[eyes[0]].w == vids[eyes[1]].w &&
		    vids[eyes[0]].h == vids[eyes[1]].h && vids[eyes[0]].w > 0) {
			li = eyes[0];
			ri = eyes[1];
			out.how = "order";
			LOGI("[LVF] no abl/abr language tags — falling back to TRACK ORDER "
			     "(track %d = left, track %d = right). If the eyes look swapped, that "
			     "is this fallback, not the renderer.",
			     vids[li].index, vids[ri].index);
		}
	}

	out.videoTracks = (int)vids.size() - out.coverTracks;
	if (li >= 0 && ri >= 0) {
		out.ok = true;
		out.leftTrack = vids[li].index;
		out.rightTrack = vids[ri].index;
		out.width = vids[li].w;
		out.height = vids[li].h;
		out.fps = vids[li].fps;
		out.durationUs = vids[li].durationUs;
		out.firstVideoPtsUs = firstSampleTimeUs(ex, out.leftTrack);
		if (vids[li].w != vids[ri].w || vids[li].h != vids[ri].h) {
			LOGE("[LVF] eye tracks differ in size (%dx%d vs %dx%d) — refusing dual",
			     vids[li].w, vids[li].h, vids[ri].w, vids[ri].h);
			out.ok = false;
		}
	}

	// ── Convergence curve ─────────────────────────────────────────────────────
	// Read unconditionally when the track is there: it costs a few hundred tiny
	// samples and it is the only chance we get (playback never touches this track).
	out.convTrack = convTrack;
	out.convMime = convMime;
	if (convTrack >= 0 && AMediaExtractor_selectTrack(ex, (size_t)convTrack) == AMEDIA_OK) {
		AMediaExtractor_seekTo(ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
		out.firstConvPtsUs = AMediaExtractor_getSampleTime(ex);
		std::vector<uint8_t> buf(kMetaSampleCap);
		int guard = 0;
		while (guard++ < kMaxConvSamples) {
			const int64_t pts = AMediaExtractor_getSampleTime(ex);
			if (pts < 0) break;
			const ssize_t sz = AMediaExtractor_readSampleData(ex, buf.data(), buf.size());
			if (sz > 0) {
				float v = 0.0f;
				if (parseConvergence((const char *)buf.data(), (size_t)sz, &v)) {
					out.convergence.push_back({pts, v});
				}
			}
			if (!AMediaExtractor_advance(ex)) break;
		}
		AMediaExtractor_unselectTrack(ex, (size_t)convTrack);
		std::sort(out.convergence.begin(), out.convergence.end(),
		          [](const LvfProbeAndroid::ConvSample &a, const LvfProbeAndroid::ConvSample &b) {
			          return a.ptsUs < b.ptsUs;
		          });
	}

	for (AMediaFormat *f : keep) AMediaFormat_delete(f);
	return out;
}

}  // namespace

float
LvfProbeAndroid::ConvergenceAt(const std::vector<ConvSample> &s, int64_t ptsUs)
{
	if (s.empty()) return 0.0f;
	if (ptsUs <= s.front().ptsUs) return s.front().value;   // clamp before the first
	if (ptsUs >= s.back().ptsUs) return s.back().value;     // clamp after the last
	// Bracketing pair, then linear interpolation. upper_bound gives the first sample
	// strictly after ptsUs, so hi >= 1 here (the <= front case returned above).
	const auto hi = std::upper_bound(s.begin(), s.end(), ptsUs,
	                                 [](int64_t t, const ConvSample &c) { return t < c.ptsUs; });
	const auto lo = hi - 1;
	const int64_t span = hi->ptsUs - lo->ptsUs;
	if (span <= 0) return lo->value;
	const float f = (float)(ptsUs - lo->ptsUs) / (float)span;
	return lo->value + (hi->value - lo->value) * f;
}

bool
LvfProbeAndroid::ApplyConvergenceForName(const std::string &name)
{
	std::string lower;
	lower.reserve(name.size());
	for (char c : name) lower.push_back((char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
	// `_noreconv` is checked first: it contains `reconv` as a substring, so the order
	// here is load-bearing.
	if (lower.find("_noreconv") != std::string::npos) return false;
	return true;  // `_reconv` and everything unmarked: apply
}

LvfProbeAndroid::Result
LvfProbeAndroid::RunFd(int fd, int64_t offset, int64_t length)
{
	Result out;
	AMediaExtractor *ex = AMediaExtractor_new();
	if (ex == nullptr) return out;
	if (AMediaExtractor_setDataSourceFd(ex, fd, offset, length) != AMEDIA_OK) {
		LOGE("[LVF] probe: setDataSourceFd failed");
		AMediaExtractor_delete(ex);
		return out;
	}
	out = run(ex);
	AMediaExtractor_delete(ex);
	return out;
}

LvfProbeAndroid::Result
LvfProbeAndroid::RunPath(const char *path)
{
	Result out;
	const int fd = ::open(path, O_RDONLY);
	if (fd < 0) return out;
	struct stat st;
	const int64_t len = (::fstat(fd, &st) == 0) ? (int64_t)st.st_size : 0;
	out = RunFd(fd, 0, len);
	::close(fd);
	return out;
}

}  // namespace mp
