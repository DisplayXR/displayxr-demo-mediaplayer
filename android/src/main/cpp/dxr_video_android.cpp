// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0

#include "dxr_video_android.h"

#include <android/log.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <cstring>
#include <vector>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace mp {

namespace {

constexpr const char *kDxrMetaMime = "application/x-displayxr-stereo";
// A sample is 32 bytes in version 1; later versions append. Anything bigger is not ours.
constexpr size_t kSampleCap = 4096;
// One sample per frame: a 10-hour clip at 60 fps is 2.2 M samples. Bound it anyway.
constexpr int kMaxSamples = 4 * 1000 * 1000;

}  // namespace

DxrVideoAndroid::Result
DxrVideoAndroid::RunFd(int fd, int64_t offset, int64_t length)
{
	Result out;
	if (fd < 0 || offset != 0) return out;
	out.info = dxrfile::ReadVideoFd(fd);
	if (!out.info.ours || out.info.metaTrackId == 0) return out;

	AMediaExtractor *ex = AMediaExtractor_new();
	if (ex == nullptr) return out;
	if (AMediaExtractor_setDataSourceFd(ex, fd, offset, length) != AMEDIA_OK) {
		AMediaExtractor_delete(ex);
		return out;
	}
	int track = -1;
	const size_t n = AMediaExtractor_getTrackCount(ex);
	for (size_t i = 0; i < n && track < 0; ++i) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
		const char *mime = nullptr;
		if (f && AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime &&
		    std::strcmp(mime, kDxrMetaMime) == 0)
			track = (int)i;
		if (f) AMediaFormat_delete(f);
	}
	out.trackSeen = track >= 0;
	if (track >= 0 && AMediaExtractor_selectTrack(ex, (size_t)track) == AMEDIA_OK) {
		AMediaExtractor_seekTo(ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
		std::vector<uint8_t> buf(kSampleCap);
		while (out.samplesRead < kMaxSamples) {
			const int64_t pts = AMediaExtractor_getSampleTime(ex);
			if (pts < 0) break;
			const ssize_t sz = AMediaExtractor_readSampleData(ex, buf.data(), buf.size());
			if (sz > 0) {
				out.curve.Add(pts, buf.data(), (size_t)sz);
				++out.samplesRead;
			}
			if (!AMediaExtractor_advance(ex)) break;
		}
	}
	AMediaExtractor_delete(ex);
	LOGI("[DXR] camera clip: sbs=%d static shift %s%+.4f, metadata track id %u (%u samples in the file): "
	     "extractor %s it, %d read, %zu valid",
	     (int)out.info.sbs, out.info.hasConvergence ? "" : "(none) ", (double)out.info.shift,
	     out.info.metaTrackId, out.info.metaSamples, out.trackSeen ? "listed" : "did NOT list",
	     out.samplesRead, out.curve.Size());
	return out;
}

}  // namespace mp
