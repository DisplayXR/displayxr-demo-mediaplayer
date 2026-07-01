// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0

#include "video_decoder.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {
// AImageReader buffer pool depth: enough for the codec to decode ahead, plus
// the one the render thread holds + the one Vulkan still references on import.
constexpr int32_t kReaderMaxImages = 6;

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
	LOGI("VideoDecoder open (zero-copy surface): %s %dx%d", mime, width_, height_);
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
			// Pace the frame BEFORE rendering it to the surface.
			//
			// (1) WALL-CLOCK CEILING — runs ALWAYS. Caps playback at real time so a
			//     racing/garbage audio master clock (unsupported audio codec that
			//     decodes far faster than real time, e.g. some .mkv tracks) can NOT
			//     speed the video up. This is the correct-speed guarantee.
			// (2) AUDIO SYNC — only ever SLOWS video further: if the audio clock is
			//     valid and BEHIND this frame, wait for it (lip-sync). It can never
			//     push video past the wall-clock ceiling above.
			if (!decodeOneWhilePaused) {
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
			}
			const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
			// render=true → present the frame into the reader's Surface (the
			// AHardwareBuffer the render thread will sample). info.size is 0 in
			// surface mode; an EOS buffer carries no image, so don't render it.
			const bool render = info.size > 0 && !eos;
			AMediaCodec_releaseOutputBuffer(codec_, outIdx, render);
			if (render) {
				positionUs_.store(info.presentationTimeUs, std::memory_order_relaxed);
				decodeOneWhilePaused = false;  // shown the post-seek frame; hold again
			}
			if (eos) {  // loop: seek back + flush, restart the clock
				AMediaExtractor_seekTo(ex_, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
				AMediaCodec_flush(codec_);
				sawInputEOS = false;
				firstPtsUs = -1;
			}
		} else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			if (outFmt_) AMediaFormat_delete(outFmt_);
			outFmt_ = AMediaCodec_getOutputFormat(codec_);
			LOGI("output format changed: %s", AMediaFormat_toString(outFmt_));
		}
	}
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
