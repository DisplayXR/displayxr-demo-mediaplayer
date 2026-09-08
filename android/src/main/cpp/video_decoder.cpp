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
constexpr int32_t kReaderMaxImages = kVideoReaderMaxImages;

// How far ahead of the presentation clock the decode thread may run. DEFAULT 0
// -- the decoder releases a frame only once it is due.
//
// #54 shipped this at 50 ms on the theory that a cushion absorbs decode jitter.
// It does not, and it is actively harmful. Measured both ways on the same
// binary and clip: cadence is IDENTICAL (gaps 2=24 3=24, zero out-of-cadence
// either way), because the judder fix comes entirely from the CONSUMER picking
// the frame due at predictedDisplayTime -- the decode thread's release time
// stopped mattering the moment that landed. There was never jitter to absorb:
// the decode thread idles at ~2.4% CPU.
//
// What the cushion DID buy was a hazard. Running eagerly, the decoder refills a
// buffer the instant AImage_delete returns it to the pool -- while the renderer
// is still sampling it. Both eye tiles are sampled from that one buffer in a
// single drawAtlas pass, so an overwrite mid-pass shows the left eye one frame
// and the right eye the next: rapid L/R mixing, reported from the field on a
// 3840x1080 clip. 4K content hid it (a slower decoder refills more slowly, so
// the race window is narrower), which is why one-clip testing missed it.
//
// Kept as a prop so the trade can be re-measured, not so it can be re-enabled.
constexpr int64_t kLookaheadUs = 0;

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

// OUTER SAFETY CAP on how far ahead of the master, in MEDIA time, a slave may decode.
// This is deliberately NOT the primary gate any more and must never be the binding one:
// the thing that actually keeps the pair matched is consumer occupancy
// (queuedUnacquired_), because a BufferQueue with an app-controlled producer and
// consumer REPLACES a queued-but-unacquired buffer instead of blocking. A lead budget
// cannot express that constraint at all -- N frames of lead is N frames queued
// back-to-back, of which only the last survives.
//
// It stays as a backstop for the case the occupancy counter is wrong (a buffer the
// consumer never sees would otherwise leave the slave free-running), so it is sized
// well above where occupancy binds: 6 frame periods, floor 200 ms.
constexpr int64_t kSlaveLeadFloorUs = 200'000;
constexpr int kSlaveLeadFrames = 6;
// Pairing decisions logged after each open/seek when debug.dxr.mp.pair_diag is set.
constexpr int kPairDiagFrames = 60;

// How long acquireFrameByPts may spend WAITING for the slave to catch up, per call.
//
// It only ever waits once it has proof the slave is behind (it just discarded a frame
// older than the master's target) and the queue has then run dry. That combination is
// the one state the occupancy gate cannot get out of on its own: the gate lets the
// slave release only after the consumer acquires, so without this the slave advances
// exactly one frame per CALL, and a lag -- once opened by a slow first few ticks --
// never closes. Polling briefly here lets several frames through per tick instead.
//
// The budget is the render thread's to give: at 60 Hz it has ~16.7 ms, and drawAtlas
// is synchronous, so ~6 ms is a safe share that cannot push a frame past its vsync.
// Spent only while catching up; steady state never enters this path at all.
constexpr int64_t kCatchUpBudgetNs = 6'000'000;

// Numeric prop read (bisect knob); <0 = unset.
int64_t
propInt(const char *env, const char *prop, int64_t dflt)
{
	if (const char *e = std::getenv(env)) return atoll(e);
	char sp[PROP_VALUE_MAX] = {};
	if (__system_property_get(prop, sp) > 0 && sp[0]) return atoll(sp);
	return dflt;
}

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
VideoDecoder::openPath(const std::string &path, int videoTrackIndex)
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
	return openFd(fd, 0, length, videoTrackIndex);
}

bool
VideoDecoder::openFd(int fd, int64_t offset, int64_t length, int videoTrackIndex)
{
	trackIndex_ = videoTrackIndex;
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
	// trackIndex_ >= 0 = an explicitly named track (the eye tracks of a dual-track
	// file); -1 = the historical "first video track". The named track is still
	// checked to BE a video track, so a stale/garbage index fails loudly here rather
	// than configuring a codec with an audio format.
	for (size_t i = 0; i < tracks; ++i) {
		if (trackIndex_ >= 0 && (int)i != trackIndex_) continue;
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
		LOGE("no video track%s", trackIndex_ >= 0 ? " at the requested index" : "");
		return false;
	}
	// AMediaFormat_getString hands back a pointer INTO the format object, and this
	// format is deleted below (right after AMediaCodec_configure) while `mime` is
	// still used by the LOGI at the end of this function -- which printed whatever
	// happened to be in the freed block. Copy it now; `mime` itself stays valid until
	// the delete, which is all createDecoderByType needs.
	const std::string mimeStr = mime;
	frameRate_ = (float)fmtInt(trackFmt, AMEDIAFORMAT_KEY_FRAME_RATE, 0);
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
	// requireDisplayLocked_ (a dual-track pair) vetoes the kill switch: without a PTS
	// riding each buffer there is nothing to pair the two eyes on.
	legacyPacing_ = !requireDisplayLocked_ &&
	                switchOn("MEDIAPLAYER_LEGACY_PACING", "debug.dxr.mp.legacy_pacing");
	diag_ = switchOn("MEDIAPLAYER_PACING_DIAG", "debug.dxr.mp.diag");
	// Bisect knob: 0 = decode thread releases a frame only once it is DUE (the
	// legacy release cadence, but still consumer-selected), >0 = run that many
	// ms ahead. Isolates "decoder runs ahead" from "consumer picks the frame".
	lookaheadUs_ = propInt("MEDIAPLAYER_LOOKAHEAD_MS", "debug.dxr.mp.lookahead_ms",
	                       kLookaheadUs / 1000) * 1000;
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
	lastPresentedPtsUs_.store(-1, std::memory_order_relaxed);
	pairedFrames_.store(0, std::memory_order_relaxed);
	unpairedFrames_.store(0, std::memory_order_relaxed);
	unpairedOld_.store(0, std::memory_order_relaxed);
	unpairedFuture_.store(0, std::memory_order_relaxed);
	slaveLeadObservedUs_.store(0, std::memory_order_relaxed);
	queuedUnacquired_.store(0, std::memory_order_relaxed);
	pairDiag_ = switchOn("MEDIAPLAYER_PAIR_DIAG", "debug.dxr.mp.pair_diag");
	pairDiagLeft_.store(pairDiag_ ? kPairDiagFrames : 0, std::memory_order_relaxed);
	pairDiagReleased_ = 0;
	pairPtsWarned_ = false;
	{
		const double fps = frameRate_ > 1.0f ? (double)frameRate_ : 30.0;
		slaveLeadUs_ = (int64_t)(kSlaveLeadFrames * 1e6 / fps);
		if (slaveLeadUs_ < kSlaveLeadFloorUs) slaveLeadUs_ = kSlaveLeadFloorUs;
	}
	if (slave_) {
		LOGI("[PAIR] slave lead budget %lld us (fps=%.2f, %d frame periods, floor %lld) "
		     "master=%p diag=%d",
		     (long long)slaveLeadUs_, (double)frameRate_, kSlaveLeadFrames,
		     (long long)kSlaveLeadFloorUs, (const void *)pacingMaster_, (int)pairDiag_);
	}
	LOGI("VideoDecoder open (zero-copy surface): %s %dx%d track=%d%s", mimeStr.c_str(), width_,
	     height_, videoTrack, slave_ ? " SLAVE (right eye; no pacing, no drops)" : "");
	LOGI("#54: frame pacing: %s (MEDIAPLAYER_LEGACY_PACING / debug.dxr.mp.legacy_pacing; 1 = pre-fix)"
	     "  lookahead=%lld ms  pool=%d",
	     legacyPacing_ ? "LEGACY sleep-in-decode-thread" : "display-locked (predictedDisplayTime)",
	     (long long)(lookaheadUs_ / 1000), (int)kReaderMaxImages);
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
			// The last PRESENTED pts is now a fact about the other side of the jump.
			// Leaving it set would have a slave throttle itself against a reference the
			// master has abandoned -- after a backward seek that reference is in the
			// future, so the slave would run away instead of following. Clearing it
			// hands the slave the seek TARGET (positionUs_, just stored) instead.
			lastPresentedPtsUs_.store(-1, std::memory_order_relaxed);
			// Whatever was in flight belongs to the other side of the flush. Clearing
			// this can only make the gate briefly permissive (the consumer decrements
			// are clamped at 0), which costs at most one extra queued frame right where
			// discard(old) is expected anyway -- whereas NOT clearing it can leave the
			// gate stuck closed on a buffer that will never be acquired.
			queuedUnacquired_.store(0, std::memory_order_relaxed);
			if (pairDiag_) pairDiagLeft_.store(kPairDiagFrames, std::memory_order_relaxed);
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
			if (slave_) {
				// ── SLAVE (right eye): STATION-KEEPING, not pacing. ──
				// No wall clock, no drops, no say in which frame is shown -- but
				// it does not get to wander either. Left truly free-running it
				// outpaces the master and, after its own EOS loop, laps it; the
				// master's target then falls in a gap between the frames still
				// queued and the pair stops matching. (The AImageReader pool is
				// NOT sufficient backpressure on its own: the consumer drains it
				// every master frame while hunting for a twin, so it rarely stays
				// full long enough to stall the codec.)
				//
				// Reference is the master's last PRESENTED pts. Before it has
				// presented anything -- pre-roll, and the first frames out of a
				// seek -- fall back to its position (a seek publishes the target
				// immediately), so the slave pre-rolls to the same place instead
				// of stalling until the master produces its first frame.
				// GATE 1 (primary): consumer occupancy. Release only when the reader
				// has nothing of ours left unacquired, so queueBuffer can never
				// replace a frame the consumer has not seen.
				{
					int guard = 0;
					for (; guard < 2000; ++guard) {
						if (stop_.load(std::memory_order_relaxed)) break;
						if (paused_.load(std::memory_order_relaxed)) break;
						if (seekRequestUs_.load(std::memory_order_relaxed) >= 0) break;
						if (queuedUnacquired_.load(std::memory_order_relaxed) < 1) break;
						// 1 ms, not 2: the consumer's catch-up poll waits on this
						// release->decode->queue round trip, so the wake-up granularity
						// bounds how many frames it can recover per render tick.
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					if (guard >= 2000) {
						// Four seconds with a frame the consumer never took. Proceed
						// anyway (a stalled right eye is worse than a replaced frame),
						// but say so: it means the counter and the queue disagree.
						static bool warned = false;
						if (!warned) {
							warned = true;
							LOGE("[PAIR] occupancy gate timed out with %d unacquired — the "
							     "counter and the BufferQueue disagree; releasing anyway",
							     queuedUnacquired_.load(std::memory_order_relaxed));
						}
						queuedUnacquired_.store(0, std::memory_order_relaxed);
					}
				}
				// GATE 2 (backstop): the PTS-lead cap. Sized so it never binds first.
				if (pacingMaster_ != nullptr) {
					for (int guard = 0; guard < 2000; ++guard) {
						if (stop_.load(std::memory_order_relaxed)) break;
						if (paused_.load(std::memory_order_relaxed)) break;
						// A pending seek retargets us entirely; the frame in hand
						// is about to be flushed, so stop waiting on it.
						if (seekRequestUs_.load(std::memory_order_relaxed) >= 0) break;
						int64_t ref = pacingMaster_->lastPresentedPtsUs();
						// Budget while the master has NOT yet presented -- pre-roll, and
						// the frames right after a seek. Two reasons it must be looser
						// than the steady-state one, both real on this content:
						//  * positionUs_ is a SEEK TARGET, not a presented PTS, and the
						//    decoder lands on the nearest preceding sync sample, so the
						//    two differ by up to a GOP.
						//  * the two timelines can have different ORIGINS. A LeiaCam2 v1
						//    file's video traks carry an initial empty edit, so PTS start
						//    at 167800 while position starts at 0 -- with only the
						//    steady-state budget the slave would stall on its very first
						//    frame waiting for a master frame it has already got.
						// Bounded, so this is a grace period rather than a hole.
						int64_t budget = slaveLeadUs_;
						if (ref < 0) {
							ref = (int64_t)(pacingMaster_->positionSeconds() * 1e6);
							budget = slaveLeadUs_ + 1'000'000;
						}
						if (ref < 0) break;  // master not open yet: do not throttle
						const int64_t lead = info.presentationTimeUs - ref;
						slaveLeadObservedUs_.store(lead, std::memory_order_relaxed);
						if (lead <= budget) break;
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
					}
				}
				if (pairDiag_ && (pairDiagReleased_++ % 30) == 0) {
					const int64_t ref = pacingMaster_ ? pacingMaster_->lastPresentedPtsUs() : -1;
					LOGI("[PAIR] slave released pts=%lld lead=%lld q=%d (master_pts=%lld "
					     "cap=%lld)",
					     (long long)info.presentationTimeUs,
					     (long long)(ref >= 0 ? info.presentationTimeUs - ref : 0),
					     queuedUnacquired_.load(std::memory_order_relaxed), (long long)ref,
					     (long long)slaveLeadUs_);
				}
			} else if (!decodeOneWhilePaused && legacyPacing_) {
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
					if (aheadUs <= lookaheadUs_) break;
					std::this_thread::sleep_for(std::chrono::microseconds(
					    std::min<int64_t>(aheadUs - lookaheadUs_, 10'000)));
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
				// Only a slave gates on this; the master's pacing already guarantees
				// one-in-flight and it never calls acquireFrameByPts to decrement it.
				if (slave_) queuedUnacquired_.fetch_add(1, std::memory_order_relaxed);
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
		// The pairing key for a dual-track file. Distinct from positionUs_, which a
		// seek moves BEFORE any frame has come out of the flush -- pairing on that
		// would ask the slave for a frame the master is not showing.
		lastPresentedPtsUs_.store(heldTsNs / 1000, std::memory_order_relaxed);
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
VideoDecoder::acquireFrameByPts(int64_t ptsUs, int *width, int *height)
{
	if (reader_ == nullptr || ptsUs < 0) return nullptr;

	// Rounding tolerance ONLY. The PTS makes a round trip through
	// releaseOutputBufferAtTime (us -> ns) and AImage_getTimestamp (ns -> us), which
	// is exact for our values; 1 us of slack costs nothing and covers a container
	// whose two tracks round a shared 90 kHz tick differently. It is NOT a
	// nearest-frame search: anything wider would silently paper over a real one-frame
	// desync between the eyes, which is the single failure this whole path exists to
	// prevent.
	constexpr int64_t kPairSlackUs = 1;
	const bool diag = pairDiag_ && pairDiagLeft_.load(std::memory_order_relaxed) > 0;
	auto diagLine = [&](int64_t pendUs, int64_t d, const char *what) {
		if (!diag) return;
		pairDiagLeft_.fetch_sub(1, std::memory_order_relaxed);
		LOGI("[PAIR] target=%lld pending=%lld d=%lld -> %s", (long long)ptsUs, (long long)pendUs,
		     (long long)d, what);
	};

	// Already holding the right frame (the master did not advance this tick).
	if (heldImage_ != nullptr) {
		int64_t tsNs = 0;
		if (AImage_getTimestamp(heldImage_, &tsNs) == AMEDIA_OK) {
			const int64_t d = tsNs / 1000 - ptsUs;
			if (d >= -kPairSlackUs && d <= kPairSlackUs) {
				AHardwareBuffer *ahb = nullptr;
				if (AImage_getHardwareBuffer(heldImage_, &ahb) == AMEDIA_OK && ahb != nullptr) {
					if (width) *width = width_;
					if (height) *height = height_;
					return ahb;
				}
			}
		}
	}

	// Drain toward the target. acquireNextImage, never acquireLatest: the frame we
	// want may already be sitting BEHIND a newer one in the queue, and acquireLatest
	// would throw it away along with every frame the master has yet to reach.
	//
	// `behind` records that we discarded a frame older than the target, i.e. the slave
	// is demonstrably lagging. Only then is it worth waiting for more (see
	// kCatchUpBudgetNs); if the queue runs dry with the slave level or ahead, there is
	// nothing to wait FOR and we return immediately.
	bool behind = false;
	const int64_t catchUpDeadlineNs = nowMonoNs() + kCatchUpBudgetNs;
	for (int guard = 0; guard < 4096; ++guard) {
		if (pendingImage_ == nullptr) {
			AImage *img = nullptr;
			if (AImageReader_acquireNextImage(reader_, &img) != AMEDIA_OK || img == nullptr) {
				if (behind && nowMonoNs() < catchUpDeadlineNs) {
					// The acquire above is what unblocks the slave's occupancy gate, so
					// the next frame is decoding right now. Give it a moment rather
					// than returning and waiting a whole render tick for it.
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				break;  // nothing more queued -- keep showing the previous right eye
			}
			pendingImage_ = img;
			// One of ours came out of the queue: the producer may release again.
			// Clamped at 0 because a flush resets the counter while items acquired
			// after it are still arriving; letting it go negative would hold the gate
			// open for exactly as many frames as it undershot.
			int q = queuedUnacquired_.load(std::memory_order_relaxed);
			while (q > 0 && !queuedUnacquired_.compare_exchange_weak(q, q - 1,
			                                                        std::memory_order_relaxed)) {
			}
		}
		int64_t tsNs = 0;
		if (AImage_getTimestamp(pendingImage_, &tsNs) != AMEDIA_OK || tsNs <= 0) {
			// No usable timestamp: it can never be matched, so it can only clog the
			// pool. Drop it and look at the next.
			diagLine(-1, 0, "discard(no-ts)");
			AImage_delete(pendingImage_);
			pendingImage_ = nullptr;
			continue;
		}
		// Same screen the master runs (validatePtsOnce): did OUR media PTS actually
		// survive the BufferQueue, or did a vendor queue substitute a system
		// timestamp? If it did not survive, exact pairing can never match and the
		// right eye would freeze forever -- so say so once, loudly, and degrade to
		// "newest available" rather than to a frozen picture.
		validatePtsOnce(tsNs);
		if (!ptsSelectable_) {
			if (!pairPtsWarned_) {
				pairPtsWarned_ = true;
				LOGE("[PAIR] the right eye's buffers do NOT carry our media PTS — exact "
				     "pairing is impossible on this device; falling back to newest-frame, "
				     "so the eyes may be up to a frame apart");
			}
			if (heldImage_ != nullptr) AImage_delete(heldImage_);
			heldImage_ = pendingImage_;
			pendingImage_ = nullptr;
			continue;  // keep draining: we want the NEWEST, not the oldest
		}
		const int64_t d = tsNs / 1000 - ptsUs;
		if (d >= -kPairSlackUs && d <= kPairSlackUs) {  // the twin
			diagLine(tsNs / 1000, d, "match");
			if (heldImage_ != nullptr) AImage_delete(heldImage_);
			heldImage_ = pendingImage_;
			pendingImage_ = nullptr;
			pairedFrames_.fetch_add(1, std::memory_order_relaxed);
			AHardwareBuffer *ahb = nullptr;
			if (AImage_getHardwareBuffer(heldImage_, &ahb) != AMEDIA_OK || ahb == nullptr) {
				LOGE("[LVF] right eye: AImage_getHardwareBuffer failed");
				return nullptr;
			}
			if (width) *width = width_;
			if (height) *height = height_;
			return ahb;
		}
		if (d < 0) {  // older than the master: it will never be shown
			diagLine(tsNs / 1000, d, "discard(old)");
			AImage_delete(pendingImage_);
			pendingImage_ = nullptr;
			unpairedFrames_.fetch_add(1, std::memory_order_relaxed);
			unpairedOld_.fetch_add(1, std::memory_order_relaxed);
			behind = true;  // proof the slave is lagging: worth waiting for the next
			continue;
		}
		// NEWER than the master. Normally that is read-ahead and we hold it for a
		// later call -- but a frame a whole clip-length ahead is not read-ahead, it is
		// left over from BEFORE a flush (the master wrapped at EOF and main re-seeked
		// us to 0 while the reader still queued end-of-clip frames). Holding one of
		// those would wedge the right eye forever, since the master's PTS only ever
		// approaches it from below and it is bounded by the pool depth how many can be
		// drained. Discard beyond kStaleUs; keep anything closer.
		if (d > kStaleUs) {
			diagLine(tsNs / 1000, d, "discard(future)");
			AImage_delete(pendingImage_);
			pendingImage_ = nullptr;
			unpairedFrames_.fetch_add(1, std::memory_order_relaxed);
			unpairedFuture_.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		diagLine(tsNs / 1000, d, "hold");
		break;  // legitimately ahead: keep it pending for a later master PTS
	}
	if (!ptsSelectable_ && heldImage_ != nullptr) {
		// Degraded path: newest-available. Nothing matched by PTS because nothing can.
		AHardwareBuffer *ahb = nullptr;
		if (AImage_getHardwareBuffer(heldImage_, &ahb) == AMEDIA_OK && ahb != nullptr) {
			if (width) *width = width_;
			if (height) *height = height_;
			return ahb;
		}
	}
	diagLine(-1, 0, "empty");
	return nullptr;  // caller keeps the right eye it already has
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
	lastPresentedPtsUs_.store(-1, std::memory_order_relaxed);
	queuedUnacquired_.store(0, std::memory_order_relaxed);
	// Per-STREAM modes, cleared with the stream. Both are set again before the next
	// open by whoever wants them; leaving requireDisplayLocked_ latched would silently
	// disable the MEDIAPLAYER_LEGACY_PACING kill switch for every clip opened after
	// the first dual one, which is exactly the kind of sticky state that makes a
	// kill switch untrustworthy.
	requireDisplayLocked_ = false;
	slave_ = false;
	trackIndex_ = -1;
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
