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

struct VideoDecoder {
	~VideoDecoder() { stop(); }

	// Open from a filesystem path (app-readable, e.g. externalDataPath) or a
	// content fd (from the SAF picker). Starts the decode thread.
	bool openPath(const std::string &path);
	bool openFd(int fd, int64_t offset, int64_t length);
	void stop();

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

	// Frames dropped because they were already past due when the render thread
	// looked (cumulative). A healthy stream holds this at 0.
	uint32_t droppedLate() const { return droppedLate_.load(std::memory_order_relaxed); }

	// True when the pre-fix sleep-in-the-decode-thread pacing is in force; the
	// render thread must then use acquireLatestBuffer() (no PTS on the buffers).
	bool legacyPacing() const { return legacyPacing_; }

private:
	bool start();
	void decodeLoop();

	AMediaExtractor *ex_ = nullptr;
	AMediaCodec *codec_ = nullptr;
	AMediaFormat *outFmt_ = nullptr;  // cached on FORMAT_CHANGED
	AImageReader *reader_ = nullptr;  // decoder output surface (GPU AHardwareBuffers)
	ANativeWindow *window_ = nullptr; // reader_'s producer surface (owned by reader_)
	int ownedFd_ = -1;                // fd we opened (path) or were handed (SAF); closed in stop()
	std::thread thread_;
	std::atomic<bool> stop_{false};
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
	// The audio clock is the PTS just WRITTEN into AAudio, so it leads audible
	// playback by the stream's buffer depth. We slew to kill drift only, against
	// the offset captured at anchor time -- imposing the audio clock's absolute
	// value would shift A/V alignment, which is not this fix's business.
	int64_t audioOffsetUs_ = 0;
	bool audioOffsetValid_ = false;
	// Whether the PTS we handed to releaseOutputBufferAtTime actually came back
	// through the BufferQueue. If a vendor queue overrode it we must NOT select
	// against the nonsense value -- that would stall the picture silently.
	bool ptsSelectable_ = true;
	bool ptsChecked_ = false;
	bool legacyPacing_ = false;  // MEDIAPLAYER_LEGACY_PACING / debug.dxr.mp.legacy_pacing

	// media time at monoNs; caller holds clockMx_.
	int64_t mediaUsLocked(int64_t monoNs) const;
	// DECODE-THREAD: nudge the presentation clock so it cannot drift away from
	// the audio clock. No-op without audio or before the clock is anchored.
	void slewToAudio();
	// RENDER-THREAD: first-frame check that the buffer timestamp is our media
	// PTS and not something the BufferQueue substituted; clears ptsSelectable_.
	void validatePtsOnce(int64_t tsNs);
};
