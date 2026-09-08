// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// VideoDecoder — Android port of David's media/VideoDecoder, swapping FFmpeg for
// the framework-native AMediaExtractor + AMediaCodec (libmediandk). ZERO-COPY
// path: the codec decodes directly into an AImageReader's Surface (the SoC
// decoder writes vendor-YUV into GPU-sampleable AHardwareBuffers — no CPU plane
// copy), and the render thread pulls the latest AHardwareBuffer and imports it
// into Vulkan (SbsRenderer::setVideoAhb) where an immutable VkSamplerYcbcr-
// Conversion does the YUV->RGB convert + per-eye downscale. Decode + pacing run
// on a background thread; the buffer hand-off is via AImageReader. Loops at EOF.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct AMediaExtractor;
struct AMediaCodec;
struct AMediaFormat;
struct AImageReader;
struct AImage;
struct AHardwareBuffer;
struct ANativeWindow;

// AImageReader pool depth. PUBLIC because the renderer's per-AHB import cache
// must stay LARGER than this -- the reader hands out up to this many distinct
// AHardwareBuffers, and an import cache smaller than that evicts (and destroys)
// an import the descriptor set still points at, every frame. Raising this from
// 6 to 10 for #54's lookahead silently broke that invariant against a hardcoded
// cache of 8, which showed up as heavy flicker. Derive, never re-type.
inline constexpr int32_t kVideoReaderMaxImages = 10;

struct VideoDecoder {
	~VideoDecoder() { stop(); }

	// Open from a filesystem path (app-readable, e.g. externalDataPath) or a
	// content fd (from the SAF picker). Starts the decode thread.
	//
	// `videoTrackIndex` picks WHICH video track to decode; -1 (the default, and the
	// only value any pre-LVF caller passes) keeps the historical behaviour of taking
	// the first `video/*` track the extractor lists. A two-track stereo file ("LVF
	// v2", one full view per track) opens TWO decoders on the same file, one per eye,
	// naming the track each should take.
	bool openPath(const std::string &path, int videoTrackIndex = -1);
	bool openFd(int fd, int64_t offset, int64_t length, int videoTrackIndex = -1);
	void stop();

	// ── Slave mode (right eye of a dual-track file). Call BEFORE open. ──
	// A slave decodes as fast as the AImageReader pool lets it and NEVER paces or
	// drops: it has no clock of its own and no say in timing. Which frame is right is
	// decided entirely by the master's presented PTS, through acquireFrameByPts().
	// Pacing a second decoder against a second clock is precisely how you get the two
	// eyes a frame apart, which reads as a flicker/shimmer rather than as depth.
	// Implies requireDisplayLockedPacing() -- a slave cannot use the legacy path.
	void setSlave(bool slave) { slave_ = slave; if (slave) requireDisplayLocked_ = true; }
	bool isSlave() const { return slave_; }

	// The decoder a slave throttles itself against. Call before open, on the slave.
	//
	// "No pacing" was too literal. A slave with no reference at all free-runs, and the
	// only thing stopping it is the AImageReader pool -- but the consumer DRAINS that
	// pool every master frame while it hunts for a twin, so the pool never stays full
	// and the codec never blocks for long. The slave then wanders arbitrarily far from
	// the master (ahead, and after its own EOS loop, lapping it), and the master's
	// target lands in a gap: older frames get discarded, the next newer one gets held,
	// and the pair almost never matches.
	//
	// So the slave still does no CLOCK work -- it never sleeps against wall time, never
	// drops, and never decides which frame is shown -- but it does keep station: it
	// will not run more than a few frame periods of MEDIA time ahead of whatever the
	// master last presented. That is the smallest amount of coupling that makes the
	// exact-PTS pairing reachable, and it leaves every timing decision with the master.
	void setPacingMaster(const VideoDecoder *master) { pacingMaster_ = master; }

	// Dual-track playback needs a PTS on every buffer (that is how the eyes are
	// paired), so the MEDIAPLAYER_LEGACY_PACING kill switch must not apply to it.
	// Call before open on the master of a dual pair.
	void requireDisplayLockedPacing() { requireDisplayLocked_ = true; }

	bool isOpen() const { return open_.load(std::memory_order_relaxed); }
	int width() const { return width_; }
	int height() const { return height_; }

	// ── Transport (thread-safe; the decode thread applies them). ──
	// Freezes/resumes the presentation clock as well as the decode thread, so a
	// pause does not leave the media clock running ahead of the picture.
	void togglePaused();
	bool paused() const { return paused_.load(std::memory_order_relaxed); }
	double positionSeconds() const { return positionUs_.load(std::memory_order_relaxed) / 1e6; }
	double durationSeconds() const { return durationUs_ / 1e6; }
	// Scrub by a relative offset (drag). Clamped to [0, duration] on the decode thread.
	void seekRelative(double deltaSeconds);
	// Seek to an absolute position (scrub bar). Clamped to [0, duration].
	void seekTo(double seconds);
	// Optional A/V master clock (audio position, seconds; <0 = unavailable). When set
	// and >=0, the decode thread presents each frame when the clock reaches its PTS
	// instead of using its own wall clock. Set once before openPath/openFd.
	void setMasterClock(double (*fn)(void *), void *ctx) { masterClock_ = fn; masterCtx_ = ctx; }

	// RENDER-THREAD: acquire the newest decoded frame's AHardwareBuffer, or
	// nullptr if no NEW frame has arrived since the last call (caller keeps
	// displaying the previous one). The decoder holds a reference to the
	// returned buffer until the next acquireLatestBuffer()/stop(); the renderer
	// takes its own AHardwareBuffer_acquire() on import, so it stays valid even
	// across the hand-off. Outputs the frame dims when non-null.
	AHardwareBuffer *acquireLatestBuffer(int *width, int *height);

	// RENDER-THREAD, display-locked path (#54). Given the display time the caller
	// is about to submit for (XrFrameState::predictedDisplayTime, monotonic ns),
	// return the newest decoded frame whose PTS is due at that instant, or
	// nullptr if the frame already on screen is still the right one. Frames that
	// fell behind are dropped here rather than shown late, and frames that are
	// not due yet are held back -- so which video frame lands on which vsync is
	// decided against the real display timeline instead of a sleep in the decode
	// thread. Requires the decoder to have been fed by the non-legacy path (the
	// PTS rides on the buffer via AMediaCodec_releaseOutputBufferAtTime).
	AHardwareBuffer *acquireFrameForDisplayTime(int64_t displayTimeNs, int *width, int *height);

	// PTS (us) of the frame acquireFrameForDisplayTime() last PROMOTED, or -1 before
	// the first one. This is the pairing key for a dual-track file: it is the PTS of
	// the buffer the caller is holding right now, not the position (positionUs_ also
	// moves on a seek, before anything has been shown).
	int64_t lastPresentedPtsUs() const { return lastPresentedPtsUs_.load(std::memory_order_relaxed); }

	// RENDER-THREAD, SLAVE ONLY: the right eye of the frame the master just returned.
	// Drains the reader with acquireNextImage (never acquireLatest -- every frame must
	// be looked at, because the one we want may be behind a newer one), discarding
	// frames older than `ptsUs`, and returns the frame whose PTS EQUALS it. A frame
	// newer than the target is kept pending for a later call and nullptr is returned,
	// which means "the right eye you are already showing is still the right one".
	//
	// Exact equality (±1 us for rounding) is deliberate: in a two-track file every
	// left sample has a right sample at the identical PTS, so a nearest-match rule
	// would only ever hide a real desync -- one eye a frame ahead of the other.
	AHardwareBuffer *acquireFrameByPts(int64_t ptsUs, int *width, int *height);

	// Right-eye frames discarded because the master had already moved past them
	// (cumulative), and right-eye frames that paired exactly. A healthy dual stream
	// grows `paired` once per shown frame and holds `unpaired` flat after warm-up.
	uint32_t pairedFrames() const { return pairedFrames_.load(std::memory_order_relaxed); }
	uint32_t unpairedFrames() const { return unpairedFrames_.load(std::memory_order_relaxed); }
	// The discard total SPLIT BY SIGN, which is the measurement that says WHICH way
	// the pairing is failing -- and the two causes have opposite fixes:
	//   old — the twin arrived AFTER the master had already moved past it: the slave
	//         is running BEHIND (decode-limited), and no amount of throttling helps.
	//   fut — the twin was a whole clip-length AHEAD: the slave is running ahead and
	//         lapping (its own EOS loop restarts it), which throttling does fix.
	uint32_t unpairedOld() const { return unpairedOld_.load(std::memory_order_relaxed); }
	uint32_t unpairedFuture() const { return unpairedFuture_.load(std::memory_order_relaxed); }
	// Media time the slave is running ahead of its master, sampled on release. Signed:
	// negative means it is BEHIND, which throttling cannot cure.
	int64_t slaveLeadUs() const { return slaveLeadObservedUs_.load(std::memory_order_relaxed); }
	// Slave frames queued into the AImageReader that the consumer has not acquired yet.
	// The gate that actually keeps the pair matched -- see queuedUnacquired_.
	int queuedUnacquired() const { return queuedUnacquired_.load(std::memory_order_relaxed); }

	// Frames dropped because they were already past due when the render thread
	// looked (cumulative). A healthy stream holds this at 0.
	uint32_t droppedLate() const { return droppedLate_.load(std::memory_order_relaxed); }

	// True when the pre-fix sleep-in-the-decode-thread pacing is in force; the
	// render thread must then use acquireLatestBuffer() (no PTS on the buffers).
	bool legacyPacing() const { return legacyPacing_; }

private:
	bool start();
	void decodeLoop();

	int trackIndex_ = -1;   // requested video track (-1 = first video track)
	bool slave_ = false;    // no pacing, no drops; frames served by PTS on request
	bool requireDisplayLocked_ = false;  // see requireDisplayLockedPacing()
	std::atomic<int64_t> lastPresentedPtsUs_{-1};
	std::atomic<uint32_t> pairedFrames_{0};
	std::atomic<uint32_t> unpairedFrames_{0};
	std::atomic<uint32_t> unpairedOld_{0};
	std::atomic<uint32_t> unpairedFuture_{0};
	std::atomic<int64_t> slaveLeadObservedUs_{0};
	// ── The real pairing constraint: CONSUMER OCCUPANCY, not PTS lead. ──
	// AImageReader's BufferQueue has an app-controlled producer AND consumer, so
	// queueBuffer takes the "cannot block" path: queuing a second item while the first
	// is still unacquired REPLACES it rather than blocking the producer. Frames then
	// vanish between releaseOutputBufferAtTime and acquireNextImage -- silently, with
	// no error and no drop counter anywhere, which is why this looked like a pairing
	// bug rather than a delivery bug. (Measured on the v1 file: consecutive acquired
	// PTS 14 frames apart, then 3, with zero old/future discards.)
	//
	// The MASTER is immune by construction and always was: its display-locked pacing
	// releases exactly one frame per frame period, so a second frame never sits behind
	// an unacquired one. Nothing about it needs to change.
	//
	// A slave therefore releases only while this is 0: +1 on each release, -1 on each
	// successful acquire, reset on flush/seek/open. It is the whole reason a PTS-lead
	// budget was never going to be sufficient -- three frames of lead is three frames
	// queued back-to-back, of which only the last survives.
	std::atomic<int> queuedUnacquired_{0};
	const VideoDecoder *pacingMaster_ = nullptr;
	int64_t slaveLeadUs_ = 100'000;  // derived from the frame rate in start()
	float frameRate_ = 0.0f;         // container frame rate, 0 = not stated
	// debug.dxr.mp.pair_diag: log the first kPairDiagFrames pairing decisions after
	// each open/seek, plus a periodic line from the slave's decode thread.
	bool pairDiag_ = false;
	std::atomic<int> pairDiagLeft_{0};
	uint32_t pairDiagReleased_ = 0;
	bool pairPtsWarned_ = false;

	AMediaExtractor *ex_ = nullptr;
	AMediaCodec *codec_ = nullptr;
	AMediaFormat *outFmt_ = nullptr;  // cached on FORMAT_CHANGED
	AImageReader *reader_ = nullptr;  // decoder output surface (GPU AHardwareBuffers)
	ANativeWindow *window_ = nullptr; // reader_'s producer surface (owned by reader_)
	int ownedFd_ = -1;                // fd we opened (path) or were handed (SAF); closed in stop()
	std::thread thread_;
	std::atomic<bool> stop_{false};
	// Set as the LAST statement of decodeLoop(). stop() cannot simply join(): the
	// decode thread may be parked inside a codec call that only completes once the
	// CONSUMER frees a buffer slot, so stop() has to keep draining while it waits, and
	// it needs a way to know the thread is done other than the join it cannot yet make.
	// Starts true so a decoder that was never opened waits for nothing.
	std::atomic<bool> threadExited_{true};
	std::atomic<bool> open_{false};
	std::atomic<bool> paused_{false};
	std::atomic<int64_t> positionUs_{0};       // last presented frame PTS
	std::atomic<int64_t> seekRequestUs_{-1};   // >=0 = pending seek target
	int64_t durationUs_ = 0;                   // from the track format (0 if unknown)
	double (*masterClock_)(void *) = nullptr;  // audio clock (A/V master), or null
	void *masterCtx_ = nullptr;
	int width_ = 0;
	int height_ = 0;

	AImage *heldImage_ = nullptr;     // most-recently-acquired image (kept alive until next acquire)
	AImage *pendingImage_ = nullptr;  // acquired but not yet due (held for a later display time)

	// ── Presentation clock (#54) ──
	// media_us(mono_ns) = anchorMediaUs_ + (mono_ns - anchorMonoNs_) / 1000.
	// Anchored on the first frame after open/seek/loop and thereafter slewed
	// gently toward the audio clock, so A/V stays locked without the audio
	// clock's ~one-AAC-frame staircase reaching the video cadence.
	// anchorMonoNs_ < 0 means the clock is unset (pre-roll) or frozen (paused);
	// in both cases the consumer takes whatever is queued.
	mutable std::mutex clockMx_;
	int64_t anchorMonoNs_ = -1;
	int64_t anchorMediaUs_ = 0;
	std::atomic<uint32_t> droppedLate_{0};
	std::atomic<uint32_t> releasedFrames_{0};  // frames the decoder handed to the reader
	std::atomic<uint32_t> shownFrames_{0};     // frames the render thread promoted
	// The audio clock is the PTS just WRITTEN into AAudio, so it leads audible
	// playback by the stream's buffer depth. We slew to kill drift only, against
	// the offset captured at anchor time -- imposing the audio clock's absolute
	// value would shift A/V alignment, which is not this fix's business.
	int64_t audioOffsetUs_ = 0;
	bool audioOffsetValid_ = false;
	int64_t lastAudioUs_ = -1;      // to spot the audio track looping independently
	int64_t lastShownMonoNs_ = -1;  // stall watchdog for the consumer's safety net
	// Whether the PTS we handed to releaseOutputBufferAtTime actually came back
	// through the BufferQueue. If a vendor queue overrode it we must NOT select
	// against the nonsense value -- that would stall the picture silently.
	bool ptsSelectable_ = true;
	bool ptsChecked_ = false;
	// XrTime is NOT CLOCK_MONOTONIC on this runtime -- measured at ~0.33 s while
	// the monotonic clock read 453258 s, i.e. it counts from runtime start. Only
	// the EPOCH differs, so one calibration against our own clock is enough; a
	// constant offset shifts latency, never cadence, which is what we are locking.
	int64_t xrEpochOffsetNs_ = 0;
	bool xrEpochCalibrated_ = false;
	bool legacyPacing_ = false;  // MEDIAPLAYER_LEGACY_PACING / debug.dxr.mp.legacy_pacing
	bool diag_ = false;          // MEDIAPLAYER_PACING_DIAG / debug.dxr.mp.diag
	int64_t lookaheadUs_ = 0;    // debug.dxr.mp.lookahead_ms (bisect knob)

	// media time at monoNs; caller holds clockMx_.
	int64_t mediaUsLocked(int64_t monoNs) const;
	// DECODE-THREAD: nudge the presentation clock so it cannot drift away from
	// the audio clock. No-op without audio or before the clock is anchored.
	void slewToAudio();
	// RENDER-THREAD: first-frame check that the buffer timestamp is our media
	// PTS and not something the BufferQueue substituted; clears ptsSelectable_.
	void validatePtsOnce(int64_t tsNs);
};
