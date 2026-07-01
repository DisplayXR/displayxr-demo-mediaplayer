// Copyright 2026, Leia Inc.
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
	void togglePaused() { paused_.store(!paused_.load(std::memory_order_relaxed)); }
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

	AImage *heldImage_ = nullptr;  // most-recently-acquired image (kept alive until next acquire)
};
