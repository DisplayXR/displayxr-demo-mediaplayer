// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0

#include "video_decoder.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {
// AImageReader buffer pool depth: enough for the codec to decode ahead, plus
// the one the render thread holds + the one Vulkan still references on import.
// Sized for the display-locked path: the frames we deliberately leave QUEUED as
// lookahead, plus the one on screen and the one held back as not-yet-due, plus
// the working set the codec needs to keep decoding. At 6 (the pre-#54 value,
// which only ever had to hold ONE frame) a 30 fps lookahead exhausted the pool
// and stalled the decoder to ~10 fps.
constexpr int32_t kReaderMaxImages = 10;

// How far ahead of the presentation clock the decode thread is allowed to run.
// This is the cushion that absorbs decode jitter: the render thread always has
// a frame in hand for the display time it is about to submit for. Bounded so we
// do not race through the file (AImageReader back-pressure bounds it in buffers
// too, but only once the pool is full).
constexpr int64_t kLookaheadUs = 50'000;

// A/V slew. Beyond kResyncUs we assume a discontinuity (seek, loop, stall) and
// re-anchor hard; below it we nudge the anchor by at most kSlewUs per frame so
// the audio clock's ~one-AAC-frame staircase (21.3 ms @ 48 kHz, 23.2 ms @
// 44.1 kHz) can never reach the video cadence.
constexpr int64_t kResyncUs = 200'000;
constexpr int64_t kSlewUs = 1'000;

// A frame legitimately sits within kLookaheadUs of the clock. Further off than
// this, in EITHER direction, and it is not a late or early frame -- it is a
// frame from the other side of a flush that the decoder has already moved past
// (at an EOS loop the decoder re-anchors to ~0 while a clip-length of pre-loop
// frames is still sitting in the reader's queue). Those get discarded.
constexpr int64_t kStaleUs = 1'000'000;

// ...but discarding must never be able to eat the whole stream. If nothing has
// reached the panel for this long while frames ARE queued, the CLOCK is what is
// wrong: re-anchor onto the frame in hand. Bounds any present or future
// clock/stream disagreement to one hiccup instead of a frozen picture.
constexpr int64_t kStallNs = 1'000'000'000LL;

int64_t
nowMonoNs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
}

// Env first (dev), then the Android property.
bool
switchOn(const char *env, const char *prop)
{
	if (const char *e = std::getenv(env)) return *e && *e != '0';
	char sp[PROP_VALUE_MAX] = {};
	if (__system_property_get(prop, sp) > 0) return sp[0] && sp[0] != '0';
	return false;
}

int32_t
fmtInt(AMediaFormat *f, const char *key, int32_t fallback)
{
	int32_t v = 0;
	return (f && AMediaFormat_getInt32(f, key, &v)) ? v : fallback;
}
}  // namespace

bool
VideoDecoder::openPath(const std::string &path)
{
	// Open the fd ourselves and use setDataSourceFd: a raw-path setDataSource
	// runs in the media extractor's own process, which can't reach our
	// app-scoped external files dir — but it can read an fd we pass it.
	int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		LOGE("open(%s) failed: %s", path.c_str(), strerror(errno));
		return false;
	}
	struct stat st;
	int64_t length = (::fstat(fd, &st) == 0) ? (int64_t)st.st_size : 0;
	return openFd(fd, 0, length);
}

bool
VideoDecoder::openFd(int fd, int64_t offset, int64_t length)
{
	ownedFd_ = fd;
	ex_ = AMediaExtractor_new();
	if (AMediaExtractor_setDataSourceFd(ex_, fd, offset, length) != AMEDIA_OK) {
		LOGE("AMediaExtractor_setDataSourceFd failed");
		AMediaExtractor_delete(ex_);
		ex_ = nullptr;
		::close(ownedFd_);
		ownedFd_ = -1;
		return false;
	}
	return start();
}

bool
VideoDecoder::start()
{
	const size_t tracks = AMediaExtractor_getTrackCount(ex_);
	int videoTrack = -1;
	AMediaFormat *trackFmt = nullptr;
	const char *mime = nullptr;
	for (size_t i = 0; i < tracks; ++i) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex_, i);
		const char *m = nullptr;
		if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
		    std::strncmp(m, "video/", 6) == 0) {
			videoTrack = (int)i;
			trackFmt = f;
			mime = m;
			break;
		}
		AMediaFormat_delete(f);
	}
	if (videoTrack < 0) {
		LOGE("no video track");
		return false;
	}
	width_ = fmtInt(trackFmt, AMEDIAFORMAT_KEY_WIDTH, 0);
	height_ = fmtInt(trackFmt, AMEDIAFORMAT_KEY_HEIGHT, 0);
	int64_t dur = 0;
	if (AMediaFormat_getInt64(trackFmt, AMEDIAFORMAT_KEY_DURATION, &dur)) durationUs_ = dur;
	AMediaExtractor_selectTrack(ex_, videoTrack);

	// ── Zero-copy output: a GPU-sampleable AImageReader Surface. The codec
	// writes its native (vendor-tiled YUV) frames straight into AHardwareBuffers
	// we later import into Vulkan — no CPU plane copy, no swscale. PRIVATE format
	// = vendor-opaque, accessible only via AImage_getHardwareBuffer (exactly what
	// the Vulkan AHB import wants). ──
	media_status_t rs = AImageReader_newWithUsage(width_, height_, AIMAGE_FORMAT_PRIVATE,
	                                              AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
	                                              kReaderMaxImages, &reader_);
	if (rs != AMEDIA_OK || reader_ == nullptr) {
		LOGE("AImageReader_newWithUsage failed (%d)", (int)rs);
		AMediaFormat_delete(trackFmt);
		return false;
	}
	if (AImageReader_getWindow(reader_, &window_) != AMEDIA_OK || window_ == nullptr) {
		LOGE("AImageReader_getWindow failed");
		AMediaFormat_delete(trackFmt);
		return false;
	}

	codec_ = AMediaCodec_createDecoderByType(mime);
	if (codec_ == nullptr) {
		LOGE("createDecoderByType(%s) failed", mime);
		AMediaFormat_delete(trackFmt);
		return false;
	}
	// Configure WITH the reader's surface → decoded frames go to AHardwareBuffers.
	if (AMediaCodec_configure(codec_, trackFmt, window_, nullptr, 0) != AMEDIA_OK) {
		LOGE("AMediaCodec_configure (surface) failed");
		AMediaFormat_delete(trackFmt);
		return false;
	}
	AMediaFormat_delete(trackFmt);
	if (AMediaCodec_start(codec_) != AMEDIA_OK) {
		LOGE("AMediaCodec_start failed");
		return false;
	}
	// Kill switch for the display-locked pacing (#54): restores the pre-fix
	// sleep-in-the-decode-thread path. diag_ adds a 1 Hz view of both clocks --
	// bring-up only, too chatty to leave on in a shipping build.
	legacyPacing_ = switchOn("MEDIAPLAYER_LEGACY_PACING", "debug.dxr.mp.legacy_pacing");
	diag_ = switchOn("MEDIAPLAYER_PACING_DIAG", "debug.dxr.mp.diag");
	{
		std::lock_guard<std::mutex> lk(clockMx_);
		anchorMonoNs_ = -1;
		anchorMediaUs_ = 0;
	}
	lastAudioUs_ = -1;
	lastShownMonoNs_ = -1;
	droppedLate_.store(0, std::memory_order_relaxed);
	releasedFrames_.store(0, std::memory_order_relaxed);
	shownFrames_.store(0, std::memory_order_relaxed);
	ptsSelectable_ = true;
	ptsChecked_ = false;
	xrEpochCalibrated_ = false;
	LOGI("VideoDecoder open (zero-copy surface): %s %dx%d", mime, width_, height_);
	LOGI("#54: frame pacing: %s (MEDIAPLAYER_LEGACY_PACING / debug.dxr.mp.legacy_pacing; 1 = pre-fix)",
	     legacyPacing_ ? "LEGACY sleep-in-decode-thread" : "display-locked (predictedDisplayTime)");
	open_.store(true, std::memory_order_relaxed);
	stop_.store(false, std::memory_order_relaxed);
	thread_ = std::thread([this] { decodeLoop(); });
	return true;
}

void
VideoDecoder::seekRelative(double deltaSeconds)
{
	if (!open_.load(std::memory_order_relaxed)) return;
	int64_t target = positionUs_.load(std::memory_order_relaxed) + (int64_t)(deltaSeconds * 1e6);
	if (target < 0) target = 0;
	if (durationUs_ > 0 && target > durationUs_) target = durationUs_;
	seekRequestUs_.store(target, std::memory_order_relaxed);
}

void
VideoDecoder::seekTo(double seconds)
{
	if (!open_.load(std::memory_order_relaxed)) return;
	int64_t target = (int64_t)(seconds * 1e6);
	if (target < 0) target = 0;
	if (durationUs_ > 0 && target > durationUs_) target = durationUs_;
	seekRequestUs_.store(target, std::memory_order_relaxed);
}

void
VideoDecoder::decodeLoop()
{
	using clock = std::chrono::steady_clock;
	auto wallStart = clock::now();
	int64_t firstPtsUs = -1;
	bool sawInputEOS = false;
	bool decodeOneWhilePaused = false;  // after a seek-while-paused, show the new frame

	while (!stop_.load(std::memory_order_relaxed)) {
		// ── seek (works even while paused: reposition + flush, then show one frame) ──
		const int64_t sk = seekRequestUs_.exchange(-1, std::memory_order_relaxed);
		if (sk >= 0) {
			AMediaExtractor_seekTo(ex_, sk, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
			AMediaCodec_flush(codec_);
			sawInputEOS = false;
			firstPtsUs = -1;
			positionUs_.store(sk, std::memory_order_relaxed);
			{  // the clock re-anchors on the first frame out of the flush
				std::lock_guard<std::mutex> lk(clockMx_);
				anchorMonoNs_ = -1;
				anchorMediaUs_ = sk;
				audioOffsetValid_ = false;
				lastAudioUs_ = -1;
			}
			decodeOneWhilePaused = paused_.load(std::memory_order_relaxed);
		}
		// ── pause: hold the current frame (don't feed/drain) unless a seek just asked
		//    for one fresh frame ──
		if (paused_.load(std::memory_order_relaxed) && !decodeOneWhilePaused) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		// ── feed input ──
		if (!sawInputEOS) {
			ssize_t inIdx = AMediaCodec_dequeueInputBuffer(codec_, 2000);
			if (inIdx >= 0) {
				size_t cap = 0;
				uint8_t *ibuf = AMediaCodec_getInputBuffer(codec_, inIdx, &cap);
				ssize_t sz = AMediaExtractor_readSampleData(ex_, ibuf, cap);
				if (sz < 0) {
					AMediaCodec_queueInputBuffer(codec_, inIdx, 0, 0, 0,
					                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
					sawInputEOS = true;
				} else {
					int64_t pts = AMediaExtractor_getSampleTime(ex_);
					AMediaCodec_queueInputBuffer(codec_, inIdx, 0, (size_t)sz, pts, 0);
					AMediaExtractor_advance(ex_);
				}
			}
		}

		// ── drain output ──
		AMediaCodecBufferInfo info;
		ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 2000);
		if (outIdx >= 0) {
			// LEGACY pacing (kill switch only — see the display-locked branch
			// below, which is the default). Paces the frame BEFORE rendering it
			// to the surface, from this thread:
			//
			// (1) WALL-CLOCK CEILING — runs ALWAYS. Caps playback at real time so a
			//     racing/garbage audio master clock (unsupported audio codec that
			//     decodes far faster than real time, e.g. some .mkv tracks) can NOT
			//     speed the video up. This is the correct-speed guarantee.
			// (2) AUDIO SYNC — only ever SLOWS video further: if the audio clock is
			//     valid and BEHIND this frame, wait for it (lip-sync). It can never
			//     push video past the wall-clock ceiling above.
			if (!decodeOneWhilePaused && legacyPacing_) {
				if (firstPtsUs < 0) {
					firstPtsUs = info.presentationTimeUs;
					wallStart = clock::now();
				}
				const int64_t targetUs = info.presentationTimeUs - firstPtsUs;
				const int64_t elapsedUs =
				    std::chrono::duration_cast<std::chrono::microseconds>(clock::now() -
				                                                          wallStart)
				        .count();
				if (targetUs > elapsedUs + 1000) {
					std::this_thread::sleep_for(
					    std::chrono::microseconds(targetUs - elapsedUs));
				}
				const double audioSec =
				    masterClock_ != nullptr ? masterClock_(masterCtx_) : -1.0;
				if (audioSec >= 0.0) {
					const double frameSec = info.presentationTimeUs / 1e6;
					for (int guard = 0; guard < 200 &&
					                    !stop_.load(std::memory_order_relaxed) &&
					                    !paused_.load(std::memory_order_relaxed) &&
					                    masterClock_(masterCtx_) + 0.005 < frameSec;
					     ++guard) {
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
					}
				}
			} else if (!decodeOneWhilePaused) {
				// ── Display-locked pacing (#54) ──
				// This thread does NOT try to hit the frame's instant. Sleeping
				// here to a free-running steady_clock and then releasing the
				// buffer "now" is what caused the judder: the release lands on
				// whichever side of a 16.67 ms vsync boundary scheduling jitter
				// puts it, so 30 fps content on a 60 Hz panel alternates 1/2/3
				// display periods per frame instead of a flat 2. Which frame is
				// shown at which display time is now decided by the render
				// thread in acquireFrameForDisplayTime(), against the display
				// time the runtime actually predicted. All this thread does is
				// anchor the clock, keep a bounded decode cushion, and slew.
				if (firstPtsUs < 0) {
					firstPtsUs = info.presentationTimeUs;
					wallStart = clock::now();
					std::lock_guard<std::mutex> lk(clockMx_);
					anchorMediaUs_ = info.presentationTimeUs;
					anchorMonoNs_ = nowMonoNs();
					audioOffsetValid_ = false;
				} else {
					slewToAudio();
				}
				// Keep at most kLookaheadUs of decoded picture ahead of the
				// clock. Short sleeps so a pause/seek/stop is still responsive.
				for (int guard = 0; guard < 1000 && !stop_.load(std::memory_order_relaxed) &&
				                    !paused_.load(std::memory_order_relaxed);
				     ++guard) {
					int64_t aheadUs = 0;
					{
						std::lock_guard<std::mutex> lk(clockMx_);
						if (anchorMonoNs_ < 0) break;  // frozen: don't throttle
						aheadUs = info.presentationTimeUs - mediaUsLocked(nowMonoNs());
					}
					if (aheadUs <= kLookaheadUs) break;
					std::this_thread::sleep_for(std::chrono::microseconds(
					    std::min<int64_t>(aheadUs - kLookaheadUs, 10'000)));
				}
			}
			const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
			// render=true → present the frame into the reader's Surface (the
			// AHardwareBuffer the render thread will sample). info.size is 0 in
			// surface mode; an EOS buffer carries no image, so don't render it.
			const bool render = info.size > 0 && !eos;
			if (render && !legacyPacing_) {
				// The timestamp rides through the BufferQueue to
				// AImage_getTimestamp(), which is how the render thread knows
				// each frame's PTS. (For an AImageReader consumer this does not
				// defer delivery the way a SurfaceFlinger latch would -- it is
				// purely the carrier. Delivery timing is the consumer's job.)
				AMediaCodec_releaseOutputBufferAtTime(codec_, outIdx,
				                                     info.presentationTimeUs * 1000);
				releasedFrames_.fetch_add(1, std::memory_order_relaxed);
			} else {
				AMediaCodec_releaseOutputBuffer(codec_, outIdx, render);
			}
			if (render) {
				// Legacy pacing presents from this thread, so this IS the shown
				// frame. Display-locked pacing runs a cushion ahead, so there the
				// position is published by the consumer instead.
				if (legacyPacing_)
					positionUs_.store(info.presentationTimeUs, std::memory_order_relaxed);
				decodeOneWhilePaused = false;  // shown the post-seek frame; hold again
			}
			if (eos) {  // loop: seek back + flush, restart the clock
				AMediaExtractor_seekTo(ex_, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
				AMediaCodec_flush(codec_);
				sawInputEOS = false;
				firstPtsUs = -1;  // re-anchors the presentation clock
			}
		} else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			if (outFmt_) AMediaFormat_delete(outFmt_);
			outFmt_ = AMediaCodec_getOutputFormat(codec_);
			LOGI("output format changed: %s", AMediaFormat_toString(outFmt_));
		}
	}
}

int64_t
VideoDecoder::mediaUsLocked(int64_t monoNs) const
{
	if (anchorMonoNs_ < 0) return anchorMediaUs_;  // unset/frozen: clock stands still
	return anchorMediaUs_ + (monoNs - anchorMonoNs_) / 1000;
}

void
VideoDecoder::slewToAudio()
{
	if (masterClock_ == nullptr) return;
	const double audioSec = masterClock_(masterCtx_);
	if (audioSec < 0.0) return;

	const int64_t audioUs = (int64_t)(audioSec * 1e6);
	const int64_t mono = nowMonoNs();
	std::lock_guard<std::mutex> lk(clockMx_);
	if (anchorMonoNs_ < 0) return;

	// The audio track loops on its own schedule, so the clock can step backwards
	// or jump. An offset captured across that boundary is a clip-length wrong:
	// throw it away and recapture rather than dragging the video clock with it.
	if (lastAudioUs_ >= 0 && (audioUs < lastAudioUs_ || audioUs - lastAudioUs_ > 1'000'000)) {
		audioOffsetValid_ = false;
	}
	lastAudioUs_ = audioUs;

	if (!audioOffsetValid_) {
		// Capture (and thereafter preserve) whatever A/V alignment the stream
		// started with, rather than snapping video onto the leading audio clock.
		audioOffsetUs_ = audioUs - mediaUsLocked(mono);
		audioOffsetValid_ = true;
		return;
	}
	const int64_t driftUs = (audioUs - audioOffsetUs_) - mediaUsLocked(mono);
	if (driftUs > kResyncUs || driftUs < -kResyncUs) {
		int64_t snapUs = audioUs - audioOffsetUs_;  // discontinuity: snap
		if (snapUs < 0) snapUs = 0;                 // never outside the media
		if (durationUs_ > 0 && snapUs > durationUs_) snapUs = durationUs_;
		anchorMediaUs_ = snapUs;
		anchorMonoNs_ = mono;
	} else {
		anchorMediaUs_ += std::clamp<int64_t>(driftUs / 8, -kSlewUs, kSlewUs);
	}
}

void
VideoDecoder::validatePtsOnce(int64_t tsNs)
{
	if (ptsChecked_) return;
	ptsChecked_ = true;
	// Media PTS starts at (or near) zero and cannot exceed the clip. A system
	// timestamp is many orders larger, which is the failure we are screening for.
	const int64_t limitUs = (durationUs_ > 0 ? durationUs_ : 24LL * 3600 * 1'000'000) + 5'000'000;
	if (tsNs / 1000 > limitUs) {
		ptsSelectable_ = false;
		LOGE("#54: buffer timestamp %lld us exceeds the media (duration %lld us) — the "
		     "BufferQueue did not carry our PTS; falling back to newest-frame selection, "
		     "cadence will NOT be display-locked",
		     (long long)(tsNs / 1000), (long long)durationUs_);
	}
}

void
VideoDecoder::togglePaused()
{
	const bool nowPaused = !paused_.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lk(clockMx_);
		if (nowPaused) {
			// Freeze the clock where it stands, so resuming does not jump the
			// picture forward by however long the pause lasted.
			anchorMediaUs_ = mediaUsLocked(nowMonoNs());
			anchorMonoNs_ = -1;
		} else if (open_.load(std::memory_order_relaxed)) {
			anchorMonoNs_ = nowMonoNs();
			audioOffsetValid_ = false;  // audio restarts from its own position
		}
	}
	paused_.store(nowPaused, std::memory_order_relaxed);
}

AHardwareBuffer *
VideoDecoder::acquireFrameForDisplayTime(int64_t displayTimeNs, int *width, int *height)
{
	if (reader_ == nullptr) return nullptr;

	const int64_t mono = nowMonoNs();
	// Put the runtime's display timeline into our clock's epoch. Calibrated once
	// (and re-calibrated if it ever drifts a second, which would mean the runtime
	// re-based its clock) -- the offset carries the wait-frame phase with it, and
	// a constant phase is exactly what we want: it shifts latency, not cadence.
	if (displayTimeNs <= 0) return nullptr;
	if (!xrEpochCalibrated_) {
		xrEpochCalibrated_ = true;
		xrEpochOffsetNs_ = mono - displayTimeNs;
		LOGI("#54: XrTime epoch offset %lld ns (predictedDisplayTime %lld vs monotonic %lld)",
		     (long long)xrEpochOffsetNs_, (long long)displayTimeNs, (long long)mono);
	}
	int64_t targetMonoNs = displayTimeNs + xrEpochOffsetNs_;
	if (targetMonoNs - mono > 1'000'000'000LL || mono - targetMonoNs > 1'000'000'000LL) {
		LOGE("#54: display timeline jumped (target %lld vs now %lld) — recalibrating",
		     (long long)targetMonoNs, (long long)mono);
		xrEpochOffsetNs_ = mono - displayTimeNs;
		targetMonoNs = mono;
	}

	int64_t targetUs = 0;
	bool clockRunning = false;
	{
		std::lock_guard<std::mutex> lk(clockMx_);
		clockRunning = anchorMonoNs_ >= 0;
		targetUs = mediaUsLocked(targetMonoNs);
	}

	// 1 Hz view of the two clocks and both ends of the queue -- the numbers that
	// say WHY a frame is or is not being shown.
	if (diag_) {
		static int64_t s_lastDiagNs = 0;
		if (mono - s_lastDiagNs > 1'000'000'000LL) {
			s_lastDiagNs = mono;
			int64_t pendPts = -1;
			if (pendingImage_ != nullptr) {
				int64_t t = 0;
				if (AImage_getTimestamp(pendingImage_, &t) == AMEDIA_OK) pendPts = t / 1000;
			}
			int64_t anchorUs = 0;
			{
				std::lock_guard<std::mutex> lk(clockMx_);
				anchorUs = anchorMediaUs_;
			}
			LOGI("#54 DIAG target=%lld pending_pts=%lld anchorMedia=%lld running=%d "
			     "released=%u shown=%u dropped=%u audio=%.3f",
			     (long long)targetUs, (long long)pendPts, (long long)anchorUs,
			     (int)clockRunning, releasedFrames_.load(std::memory_order_relaxed),
			     shownFrames_.load(std::memory_order_relaxed),
			     droppedLate_.load(std::memory_order_relaxed),
			     masterClock_ ? masterClock_(masterCtx_) : -1.0);
		}
	}

	// Walk the queue: keep the NEWEST frame that is already due, drop the ones
	// it superseded, and hold back the first frame that is not due yet (it is
	// the right frame for a later display time). Before the clock is anchored
	// (pre-roll, or paused after a seek) take whatever is there.
	bool promoted = false;
	for (int guard = 0; guard < kReaderMaxImages + 2; ++guard) {
		if (pendingImage_ == nullptr) {
			AImage *img = nullptr;
			if (AImageReader_acquireNextImage(reader_, &img) != AMEDIA_OK || img == nullptr)
				break;  // nothing more queued
			pendingImage_ = img;
		}
		// Unknown timestamp, frozen clock or unusable PTS all mean "take it" --
		// a degraded cadence is recoverable, a frozen picture is not.
		bool due = true;
		bool selecting = false;
		if (clockRunning && ptsSelectable_) {
			int64_t tsNs = 0;
			if (AImage_getTimestamp(pendingImage_, &tsNs) == AMEDIA_OK && tsNs > 0) {
				validatePtsOnce(tsNs);
				if (ptsSelectable_) {
					selecting = true;
					due = (tsNs / 1000) <= targetUs;
				}
			}
		}
		if (selecting) {
			int64_t tsNs = 0;
			if (AImage_getTimestamp(pendingImage_, &tsNs) == AMEDIA_OK) {
				const int64_t offUs = tsNs / 1000 - targetUs;
				const bool stalled =
				    lastShownMonoNs_ >= 0 && mono - lastShownMonoNs_ > kStallNs;
				if ((offUs > kStaleUs || offUs < -kStaleUs) && !stalled) {
					// Left over from before a flush -- the decoder has already
					// moved the clock past it. Drop it and look at the next.
					AImage_delete(pendingImage_);
					pendingImage_ = nullptr;
					continue;
				}
				if (!due && stalled) {
					// Nothing has reached the panel for a second while frames
					// are queued: believe the stream, not the clock.
					LOGE("#54: nothing shown for %lld ms with a frame at %lld us and the "
					     "clock at %lld us — re-anchoring onto the stream",
					     (long long)((mono - lastShownMonoNs_) / 1'000'000),
					     (long long)(tsNs / 1000), (long long)targetUs);
					std::lock_guard<std::mutex> lk(clockMx_);
					anchorMediaUs_ = tsNs / 1000;
					anchorMonoNs_ = targetMonoNs;
					audioOffsetValid_ = false;
					due = true;
				}
			}
		}
		if (!due) break;  // not due yet — keep it for a later display time
		if (heldImage_ != nullptr) AImage_delete(heldImage_);
		if (promoted && selecting) {
			// We had already taken a due frame this tick and found a newer one
			// also due: the first one never reached the panel.
			droppedLate_.fetch_add(1, std::memory_order_relaxed);
		}
		heldImage_ = pendingImage_;
		pendingImage_ = nullptr;
		promoted = true;
		lastShownMonoNs_ = mono;
		shownFrames_.fetch_add(1, std::memory_order_relaxed);
	}
	if (!promoted || heldImage_ == nullptr) {
		return nullptr;  // the frame already on screen is still the right one
	}

	int64_t heldTsNs = 0;
	if (AImage_getTimestamp(heldImage_, &heldTsNs) == AMEDIA_OK) {
		positionUs_.store(heldTsNs / 1000, std::memory_order_relaxed);
	}

	AHardwareBuffer *ahb = nullptr;
	if (AImage_getHardwareBuffer(heldImage_, &ahb) != AMEDIA_OK || ahb == nullptr) {
		LOGE("AImage_getHardwareBuffer failed");
		return nullptr;
	}
	if (width) *width = width_;
	if (height) *height = height_;
	return ahb;
}

AHardwareBuffer *
VideoDecoder::acquireLatestBuffer(int *width, int *height)
{
	if (reader_ == nullptr) return nullptr;
	AImage *img = nullptr;
	media_status_t r = AImageReader_acquireLatestImage(reader_, &img);
	if (r != AMEDIA_OK || img == nullptr) {
		return nullptr;  // nothing new — caller keeps displaying the previous buffer
	}
	// The previously-held image's AHardwareBuffer is kept alive by the renderer's
	// own AHardwareBuffer_acquire() on import, so releasing it back to the pool
	// here is safe (the GPU finished sampling it — drawAtlas waits idle).
	if (heldImage_ != nullptr) {
		AImage_delete(heldImage_);
	}
	heldImage_ = img;

	AHardwareBuffer *ahb = nullptr;
	if (AImage_getHardwareBuffer(img, &ahb) != AMEDIA_OK || ahb == nullptr) {
		LOGE("AImage_getHardwareBuffer failed");
		return nullptr;
	}
	if (width) *width = width_;
	if (height) *height = height_;
	return ahb;
}

void
VideoDecoder::stop()
{
	stop_.store(true, std::memory_order_relaxed);
	if (thread_.joinable()) thread_.join();
	if (heldImage_) {
		AImage_delete(heldImage_);
		heldImage_ = nullptr;
	}
	if (pendingImage_) {
		AImage_delete(pendingImage_);
		pendingImage_ = nullptr;
	}
	{
		std::lock_guard<std::mutex> lk(clockMx_);
		anchorMonoNs_ = -1;
		audioOffsetValid_ = false;
	}
	ptsSelectable_ = true;
	ptsChecked_ = false;
	if (codec_) {
		AMediaCodec_stop(codec_);
		AMediaCodec_delete(codec_);
		codec_ = nullptr;
	}
	if (reader_) {  // also frees window_ (owned by the reader)
		AImageReader_delete(reader_);
		reader_ = nullptr;
		window_ = nullptr;
	}
	if (ex_) {
		AMediaExtractor_delete(ex_);
		ex_ = nullptr;
	}
	if (outFmt_) {
		AMediaFormat_delete(outFmt_);
		outFmt_ = nullptr;
	}
	if (ownedFd_ >= 0) {
		::close(ownedFd_);
		ownedFd_ = -1;
	}
	open_.store(false, std::memory_order_relaxed);
}
