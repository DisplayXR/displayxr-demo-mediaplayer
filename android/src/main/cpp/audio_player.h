// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// AudioPlayer — Android audio playback for the media player. AMediaCodec (audio
// decode) + AAudio (output). Decodes the file's audio track on its own thread
// and plays it; the blocking AAudio write paces playback to real time, so the
// PTS of the buffer just written is the A/V master clock the video decoder syncs
// to (VideoDecoder::setMasterClock). Independent of VideoDecoder — its own
// extractor over the same file (a SEPARATE fd; the two extractors must not share
// a file offset). Silent no-op if the file has no audio track.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

struct AMediaExtractor;
struct AMediaCodec;
struct AAudioStreamStruct;  // AAudioStream is a typedef of this (aaudio/AAudio.h)

struct AudioPlayer {
	~AudioPlayer();  // closes the AAudio stream (kept open across clips for reuse)

	// Open from a filesystem path (opens its own fd) or a content fd (from the
	// SAF picker — the caller keeps ownership of `fd`; we dup it internally so
	// our extractor has an independent file offset from the video decoder's).
	bool openPath(const std::string &path);
	bool openFd(int fd, int64_t offset, int64_t length);
	void stop();
	bool hasAudio() const { return open_.load(std::memory_order_relaxed); }

	void setPaused(bool p);
	void seekRelative(double deltaSeconds);
	void seekTo(double seconds)
	{
		if (open_.load(std::memory_order_relaxed))
			seekRequestUs_.store((int64_t)(seconds < 0 ? 0 : seconds * 1e6),
			                     std::memory_order_relaxed);
	}

	// Playback position in seconds (PTS of the last buffer handed to AAudio), or
	// -1 when unavailable. This is the A/V master clock.
	double clockSeconds() const
	{
		int64_t c = clockUs_.load(std::memory_order_relaxed);
		return c < 0 ? -1.0 : c / 1e6;
	}
	// Thunk for VideoDecoder::setMasterClock(fn, ctx).
	static double clockThunk(void *ctx) { return static_cast<AudioPlayer *>(ctx)->clockSeconds(); }

private:
	bool startFromExtractor();  // select audio track, configure codec, open AAudio, spawn thread
	bool start(int sampleRate, int channels);
	// Open the AAudio stream, or REUSE the existing one when rate/channels match
	// (closing+reopening a SHARED stream per clip leaves the new one silent on
	// some devices — "audio works clip 1, silent clip 2"). Flushes on reuse.
	bool ensureStream(int sampleRate, int channels);
	// Tear down the per-clip media (decode thread + codec + extractor + fd) but
	// KEEP the AAudio stream alive for reuse on the next clip.
	void teardownMedia();
	void decodeLoop();

	AMediaExtractor *ex_ = nullptr;
	AMediaCodec *codec_ = nullptr;
	AAudioStreamStruct *stream_ = nullptr;  // == AAudioStream*
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> open_{false};
	std::atomic<bool> paused_{false};
	std::atomic<int64_t> clockUs_{-1};
	std::atomic<int64_t> seekRequestUs_{-1};
	int sampleRate_ = 48000;
	int channels_ = 2;
	int streamRate_ = 0;  // params the currently-open AAudio stream was created with
	int streamCh_ = 0;
	int ownedFd_ = -1;
};
