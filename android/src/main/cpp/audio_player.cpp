// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0

#include "audio_player.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

bool
AudioPlayer::openPath(const std::string &path)
{
	int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		LOGE("audio open(%s) failed", path.c_str());
		return false;
	}
	struct stat st;
	int64_t length = (::fstat(fd, &st) == 0) ? (int64_t)st.st_size : 0;
	return openFd(fd, 0, length);  // takes ownership of fd
}

bool
AudioPlayer::openFd(int fd, int64_t offset, int64_t length)
{
	// Tear down the PREVIOUS clip's media (thread/codec/extractor) but KEEP the
	// AAudio stream so it can be reused — closing+reopening the stream per clip
	// leaves the new one silent on some devices ("works clip 1, silent clip 2").
	teardownMedia();
	stop_.store(false, std::memory_order_relaxed);
	ownedFd_ = fd;
	ex_ = AMediaExtractor_new();
	if (AMediaExtractor_setDataSourceFd(ex_, fd, offset, length) != AMEDIA_OK) {
		LOGE("audio setDataSourceFd failed");
		AMediaExtractor_delete(ex_);
		ex_ = nullptr;
		::close(ownedFd_);
		ownedFd_ = -1;
		return false;
	}
	return startFromExtractor();
}

bool
AudioPlayer::startFromExtractor()
{
	const size_t tracks = AMediaExtractor_getTrackCount(ex_);
	int audioTrack = -1;
	int sr = 48000, ch = 2;
	for (size_t i = 0; i < tracks; ++i) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex_, i);
		const char *m = nullptr;
		if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
		    std::strncmp(m, "audio/", 6) == 0) {
			audioTrack = (int)i;
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
			AMediaExtractor_selectTrack(ex_, audioTrack);
			codec_ = AMediaCodec_createDecoderByType(m);
			if (codec_ == nullptr ||
			    AMediaCodec_configure(codec_, f, nullptr, nullptr, 0) != AMEDIA_OK ||
			    AMediaCodec_start(codec_) != AMEDIA_OK) {
				LOGE("audio codec init failed");
				AMediaFormat_delete(f);
				return false;
			}
			AMediaFormat_delete(f);
			break;
		}
		AMediaFormat_delete(f);
	}
	if (audioTrack < 0) {
		LOGI("no audio track — playing silent");
		return false;
	}
	return start(sr, ch);
}

bool
AudioPlayer::start(int sampleRate, int channels)
{
	if (!ensureStream(sampleRate, channels)) return false;
	open_.store(true, std::memory_order_relaxed);
	stop_.store(false, std::memory_order_relaxed);
	thread_ = std::thread([this] { decodeLoop(); });
	return true;
}

// Reuse the existing AAudio stream when the new clip's rate/channels match
// (just flush leftover audio + restart); only close+reopen on a real mismatch.
bool
AudioPlayer::ensureStream(int sampleRate, int channels)
{
	sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
	channels_ = channels > 0 ? channels : 2;

	if (stream_ != nullptr && sampleRate_ == streamRate_ && channels_ == streamCh_) {
		// Reuse: flush clip-1's tail (requires STOPPED/PAUSED), then restart.
		AAudioStream_requestStop(stream_);
		AAudioStream_requestFlush(stream_);
		aaudio_result_t rs = AAudioStream_requestStart(stream_);
		LOGI("AudioPlayer reuse stream %dHz %dch (start=%d)", streamRate_, streamCh_, (int)rs);
		channels_ = streamCh_;
		return true;
	}
	if (stream_ != nullptr) {  // params changed — must reopen
		AAudioStream_requestStop(stream_);
		AAudioStream_close(stream_);
		stream_ = nullptr;
	}

	AAudioStreamBuilder *builder = nullptr;
	if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
		LOGE("AAudio_createStreamBuilder failed");
		return false;
	}
	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setSampleRate(builder, sampleRate_);
	AAudioStreamBuilder_setChannelCount(builder, channels_);
	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
	AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
	AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MOVIE);
	aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream_);
	AAudioStreamBuilder_delete(builder);
	if (r != AAUDIO_OK || stream_ == nullptr) {
		LOGE("AAudio openStream failed: %s", AAudio_convertResultToText(r));
		return false;
	}
	AAudioStream_requestStart(stream_);
	const int actRate = AAudioStream_getSampleRate(stream_);
	const int actCh = AAudioStream_getChannelCount(stream_);
	const aaudio_format_t actFmt = AAudioStream_getFormat(stream_);
	LOGI("AudioPlayer open: requested %dHz %dch I16 → actual %dHz %dch fmt=%d state=%d", sampleRate_,
	     channels_, actRate, actCh, (int)actFmt, (int)AAudioStream_getState(stream_));
	if (actCh > 0) channels_ = actCh;
	if (actFmt != AAUDIO_FORMAT_PCM_I16)
		LOGE("AAudio granted non-I16 format %d — playback will be wrong/silent", (int)actFmt);
	streamRate_ = sampleRate_;  // remember what we opened with (request rate; ch adopted)
	streamCh_ = channels_;
	return true;
}

void
AudioPlayer::setPaused(bool p)
{
	paused_.store(p, std::memory_order_relaxed);
	if (stream_) {
		if (p) {
			AAudioStream_requestPause(stream_);
		} else {
			AAudioStream_requestStart(stream_);
		}
	}
}

void
AudioPlayer::seekRelative(double deltaSeconds)
{
	if (!open_.load(std::memory_order_relaxed)) return;
	int64_t cur = clockUs_.load(std::memory_order_relaxed);
	if (cur < 0) cur = 0;
	int64_t target = cur + (int64_t)(deltaSeconds * 1e6);
	if (target < 0) target = 0;
	seekRequestUs_.store(target, std::memory_order_relaxed);
}

void
AudioPlayer::decodeLoop()
{
	bool sawInputEOS = false;
	while (!stop_.load(std::memory_order_relaxed)) {
		// seek (works while paused)
		const int64_t sk = seekRequestUs_.exchange(-1, std::memory_order_relaxed);
		if (sk >= 0) {
			AMediaExtractor_seekTo(ex_, sk, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
			AMediaCodec_flush(codec_);
			if (stream_) AAudioStream_requestFlush(stream_);
			sawInputEOS = false;
			clockUs_.store(sk, std::memory_order_relaxed);
			if (stream_ && !paused_.load(std::memory_order_relaxed))
				AAudioStream_requestStart(stream_);
		}
		if (paused_.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		// feed
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

		// drain → AAudio (blocking write paces to real time)
		AMediaCodecBufferInfo info;
		ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 2000);
		if (outIdx >= 0) {
			if (info.size > 0) {
				size_t outSize = 0;
				uint8_t *obuf = AMediaCodec_getOutputBuffer(codec_, outIdx, &outSize);
				if (obuf != nullptr && stream_ != nullptr) {
					const int frames = (int)(info.size / (channels_ * 2));  // PCM_I16
					int written = 0;
					aaudio_result_t lastW = 0;
					while (written < frames && !stop_.load(std::memory_order_relaxed)) {
						aaudio_result_t w = AAudioStream_write(
						    stream_, obuf + (size_t)written * channels_ * 2, frames - written,
						    1'000'000'000L);
						lastW = w;
						if (w < 0) break;
						written += w;
					}
					// One-shot startup confirmation that PCM is reaching AAudio and
					// the HW is draining it (framesRead advances ⇒ audible); silent
					// thereafter to avoid log spam.
					static int dbgN = 0;
					if (dbgN++ < 2) {
						LOGI("audio: wrote=%d (%s) clk=%.2f fw=%lld fr=%lld", written,
						     lastW < 0 ? AAudio_convertResultToText(lastW) : "ok",
						     info.presentationTimeUs / 1e6,
						     (long long)AAudioStream_getFramesWritten(stream_),
						     (long long)AAudioStream_getFramesRead(stream_));
					}
					clockUs_.store(info.presentationTimeUs, std::memory_order_relaxed);
				}
			}
			const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
			AMediaCodec_releaseOutputBuffer(codec_, outIdx, false);
			if (eos) {  // loop with the video
				AMediaExtractor_seekTo(ex_, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
				AMediaCodec_flush(codec_);
				sawInputEOS = false;
				clockUs_.store(0, std::memory_order_relaxed);
			}
		} else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			// DIAG: the decoder's real output format (PCM encoding 2=I16/4=FLOAT,
			// actual sample rate/channels) — confirms our PCM_I16 assumption.
			AMediaFormat *of = AMediaCodec_getOutputFormat(codec_);
			if (of != nullptr) {
				LOGI("audio DIAG output format: %s", AMediaFormat_toString(of));
				AMediaFormat_delete(of);
			}
		}
	}
}

// Per-clip teardown: stop the decode thread + release codec/extractor/fd, but
// LEAVE the AAudio stream open so the next clip reuses it.
void
AudioPlayer::teardownMedia()
{
	stop_.store(true, std::memory_order_relaxed);
	if (thread_.joinable()) thread_.join();
	if (codec_) {
		AMediaCodec_stop(codec_);
		AMediaCodec_delete(codec_);
		codec_ = nullptr;
	}
	if (ex_) {
		AMediaExtractor_delete(ex_);
		ex_ = nullptr;
	}
	if (ownedFd_ >= 0) {
		::close(ownedFd_);
		ownedFd_ = -1;
	}
	open_.store(false, std::memory_order_relaxed);
	clockUs_.store(-1, std::memory_order_relaxed);
}

// Public stop (clip switch / session teardown): tear down the media but KEEP
// the AAudio stream open so the next clip reuses it (closing+reopening per clip
// is what left clip 2 silent). The stream is closed only at process exit (dtor).
void
AudioPlayer::stop()
{
	teardownMedia();
}

// Process exit: media + the AAudio stream itself.
AudioPlayer::~AudioPlayer()
{
	teardownMedia();
	if (stream_) {
		AAudioStream_requestStop(stream_);
		AAudioStream_close(stream_);
		stream_ = nullptr;
		streamRate_ = 0;
		streamCh_ = 0;
	}
}
