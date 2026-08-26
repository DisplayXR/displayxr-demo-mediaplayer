// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0

#include "video_stereo_probe_android.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace mp {
namespace {

// Mirrors the desktop probe's budget: stop at kVoteWant votes, never examine
// more than kMaxFrames, and do not let dark (fade-in) frames vote at all -- a
// black frame correlates with itself perfectly and would call anything stereo.
constexpr int kMaxFrames = 24;
constexpr int kDarkMean = 12;
constexpr int64_t kDequeueTimeoutUs = 20'000;
constexpr int64_t kBudgetUs = 1'500'000;  // hard stop: never hold the open path longer

// MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible. Any flexible
// 4:2:0 layout puts the luma plane first, tightly described by stride /
// slice-height, which is all AnalyzeLuma needs.
constexpr int32_t kColorFormatYUV420Flexible = 0x7F420888;

int32_t
fmtInt(AMediaFormat *f, const char *key, int32_t fallback)
{
	int32_t v = 0;
	return (f && AMediaFormat_getInt32(f, key, &v)) ? v : fallback;
}

bool
lumaIsDark(const uint8_t *y, int w, int h, ptrdiff_t stride)
{
	// 32x32 subsample is plenty to tell a fade-in from a picture.
	uint64_t acc = 0;
	int n = 0;
	for (int j = 0; j < 32; j++) {
		const uint8_t *row = y + (ptrdiff_t)((int64_t)j * (h - 1) / 31) * stride;
		for (int i = 0; i < 32; i++) {
			acc += row[(int64_t)i * (w - 1) / 31];
			n++;
		}
	}
	return n > 0 && (acc / (uint64_t)n) < (uint64_t)kDarkMean;
}

VideoStereoProbeAndroid::Result
run(AMediaExtractor *ex)
{
	VideoStereoProbeAndroid::Result out;
	const auto t0 = std::chrono::steady_clock::now();
	auto elapsed = [&] {
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
		    .count();
	};

	const size_t nTracks = AMediaExtractor_getTrackCount(ex);
	AMediaFormat *trackFmt = nullptr;
	const char *mime = nullptr;
	for (size_t i = 0; i < nTracks; i++) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
		const char *m = nullptr;
		if (f && AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
		    std::strncmp(m, "video/", 6) == 0) {
			trackFmt = f;
			mime = m;
			AMediaExtractor_selectTrack(ex, i);
			break;
		}
		if (f) AMediaFormat_delete(f);
	}
	if (trackFmt == nullptr) {
		LOGE("stereo probe: no video track");
		return out;
	}
	out.width = fmtInt(trackFmt, AMEDIAFORMAT_KEY_WIDTH, 0);
	out.height = fmtInt(trackFmt, AMEDIAFORMAT_KEY_HEIGHT, 0);

	AMediaCodec *codec = AMediaCodec_createDecoderByType(mime);
	if (codec == nullptr) {
		LOGE("stereo probe: no decoder for %s", mime);
		AMediaFormat_delete(trackFmt);
		return out;
	}
	AMediaFormat_setInt32(trackFmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatYUV420Flexible);
	if (AMediaCodec_configure(codec, trackFmt, nullptr /* no surface: buffers */, nullptr, 0) !=
	        AMEDIA_OK ||
	    AMediaCodec_start(codec) != AMEDIA_OK) {
		LOGE("stereo probe: configure/start failed");
		AMediaFormat_delete(trackFmt);
		AMediaCodec_delete(codec);
		return out;
	}
	AMediaFormat_delete(trackFmt);

	StereoVote vote;
	AMediaFormat *outFmt = nullptr;
	bool inputEos = false;
	bool sourceExhausted = false;
	while (out.framesExamined < kMaxFrames && vote.Samples() < StereoVote::kVoteWant &&
	       elapsed() < kBudgetUs) {
		if (!inputEos) {
			const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, kDequeueTimeoutUs);
			if (in >= 0) {
				size_t cap = 0;
				uint8_t *buf = AMediaCodec_getInputBuffer(codec, in, &cap);
				const ssize_t sz = AMediaExtractor_readSampleData(ex, buf, cap);
				if (sz < 0) {
					AMediaCodec_queueInputBuffer(codec, in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
					inputEos = true;
				} else {
					AMediaCodec_queueInputBuffer(codec, in, 0, (size_t)sz,
					                             AMediaExtractor_getSampleTime(ex), 0);
					AMediaExtractor_advance(ex);
				}
			}
		}
		AMediaCodecBufferInfo info;
		const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, kDequeueTimeoutUs);
		if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			if (outFmt) AMediaFormat_delete(outFmt);
			outFmt = AMediaCodec_getOutputFormat(codec);
			continue;
		}
		if (idx < 0) continue;
		if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
			AMediaCodec_releaseOutputBuffer(codec, idx, false);
			sourceExhausted = true;
			break;
		}
		if (info.size > 0) {
			if (outFmt == nullptr) outFmt = AMediaCodec_getOutputFormat(codec);
			size_t osz = 0;
			const uint8_t *y = AMediaCodec_getOutputBuffer(codec, idx, &osz);
			const int w = fmtInt(outFmt, AMEDIAFORMAT_KEY_WIDTH, out.width);
			const int h = fmtInt(outFmt, AMEDIAFORMAT_KEY_HEIGHT, out.height);
			const int stride = fmtInt(outFmt, AMEDIAFORMAT_KEY_STRIDE, w);
			// The visible picture may be a crop of the coded frame (1080 in 1088).
			int32_t cl = 0, ct = 0, cr = 0, cb = 0;
			int cw = w, ch = h;
			if (AMediaFormat_getRect(outFmt, AMEDIAFORMAT_KEY_DISPLAY_CROP, &cl, &ct, &cr, &cb)) {
				cw = cr - cl + 1;
				ch = cb - ct + 1;
			}
			if (y != nullptr && w > 0 && h > 0 && stride >= w &&
			    osz >= (size_t)stride * (size_t)(ct + ch)) {
				const uint8_t *crop = y + (ptrdiff_t)info.offset + (ptrdiff_t)ct * stride + cl;
				out.framesExamined++;
				if (!lumaIsDark(crop, cw, ch, stride)) {
					vote.Add(StereoDetect::AnalyzeLuma(crop, cw, ch, stride, 1));
				}
			}
		}
		AMediaCodec_releaseOutputBuffer(codec, idx, false);
	}
	if (vote.Samples() > 0 && !vote.Sufficient() && sourceExhausted) {
		vote.AcceptSingle();  // the clip ran out, not our budget
	}
	out.content = vote.Result();
	out.votes = vote.Samples();
	out.ok = out.framesExamined > 0;
	out.elapsedUs = elapsed();

	if (outFmt) AMediaFormat_delete(outFmt);
	AMediaCodec_stop(codec);
	AMediaCodec_delete(codec);
	LOGI("stereo probe: %dx%d frames=%d votes=%d -> %s (decided=%d conf=%.2f) in %lld ms",
	     out.width, out.height, out.framesExamined, out.votes, LayoutName(out.content.layout),
	     (int)out.content.decided, (double)out.content.confidence, (long long)(out.elapsedUs / 1000));
	return out;
}

}  // namespace

VideoStereoProbeAndroid::Result
VideoStereoProbeAndroid::RunFd(int fd, int64_t offset, int64_t length)
{
	Result out;
	AMediaExtractor *ex = AMediaExtractor_new();
	// The extractor dup()s the fd and reads with absolute offsets, so sharing
	// the caller's fd is safe (same reasoning as the audio player's own extractor).
	if (AMediaExtractor_setDataSourceFd(ex, fd, offset, length) != AMEDIA_OK) {
		LOGE("stereo probe: setDataSourceFd failed");
		AMediaExtractor_delete(ex);
		return out;
	}
	out = run(ex);
	AMediaExtractor_delete(ex);
	return out;
}

VideoStereoProbeAndroid::Result
VideoStereoProbeAndroid::RunPath(const char *path)
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
