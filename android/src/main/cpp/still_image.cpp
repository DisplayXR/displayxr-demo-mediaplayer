// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0

#include "still_image.h"

#include "LifLoader.h"
#include "MpoLoader.h"
#include "StereoDetect.h"

#include <android/log.h>

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace mp {

namespace {

// Bake the LIF's intended zero-disparity plane into the composed full-SBS
// buffer: shift each eye's half horizontally by ±0.5*conv*W px (the desktop
// applies the same 0.5-scaled per-eye shift at render time: App.cpp's
// `conv = convergence_ + 0.5*mediaConvergence_`). Exposed strips are filled by
// edge replication; a static image pays this once at load, so no shader or
// per-frame work is needed.
void
bake_lif_convergence(DecodedImage &img, float conv)
{
	const int W = img.width / 2, H = img.height;
	const int s = (int)std::lround(0.5f * conv * (float)W);
	if (s == 0) {
		return;
	}
	std::vector<uint8_t> row((size_t)W * 4);
	auto shift_half = [&](int x0, int dx) {
		for (int y = 0; y < H; ++y) {
			uint8_t *base = img.pixels.data() + ((size_t)y * img.width + x0) * 4;
			for (int x = 0; x < W; ++x) {
				int sx = x - dx;
				if (sx < 0) sx = 0;
				if (sx >= W) sx = W - 1;
				std::memcpy(row.data() + (size_t)x * 4, base + (size_t)sx * 4, 4);
			}
			std::memcpy(base, row.data(), (size_t)W * 4);
		}
	};
	shift_half(0, +s);  // left eye content moves right
	shift_half(W, -s);  // right eye content moves left
	LOGI("LIF reconvergence baked: conv=%+.4f -> ±%d px per eye", conv, s);
}

// The filename, then the pixels; `meta` is the container's say, or null.
MediaInfo
resolve(const std::string &name, MediaKind kind, const DecodedImage &image, const MediaInfo *meta)
{
	StereoLayout fromName;
	StereoDetectResult content{};
	const bool nameDecided = MediaSource::LayoutFromFilename(name, fromName);
	if (!meta && !nameDecided) {
		content = StereoDetect::AnalyzeRGBA(image.pixels.data(), image.width, image.height,
		                                    (ptrdiff_t)image.width * 4);
	}
	return MediaSource::Resolve(name, kind, image.width, image.height, nullptr, meta,
	                            (!meta && !nameDecided && content.decided) ? &content : nullptr);
}

}  // namespace

bool
StillImage::Stage(int fd, long long off, long long len, const std::string &path)
{
	FILE *out = std::fopen(path.c_str(), "wb");
	if (out == nullptr) {
		LOGE("cannot stage an image to %s", path.c_str());
		return false;
	}
	bool copy_ok = true;
	uint8_t buf[64 * 1024];
	long long pos = off;
	long long remaining = len;
	while (remaining > 0) {
		const size_t want = remaining < (long long)sizeof(buf) ? (size_t)remaining : sizeof(buf);
		const ssize_t got = pread(fd, buf, want, (off_t)pos);
		if (got <= 0 || std::fwrite(buf, 1, (size_t)got, out) != (size_t)got) {
			copy_ok = false;
			break;
		}
		pos += got;
		remaining -= got;
	}
	std::fclose(out);
	if (!copy_ok) {
		LOGE("failed to stage an image (fd=%d len=%lld)", fd, len);
	}
	return copy_ok;
}

bool
StillImage::Decode(const std::string &path, const std::string &name, StillImage &out)
{
	LifResult r;
	if (!LifLoader::IsLif(path) && MpoLoader::IsMpo(path)) {
		MpoResult mpo = MpoLoader::Load(path);
		r.image = std::move(mpo.image);
		r.layout = mpo.layout;
		r.ok = r.stereo = mpo.ok;
	}
	if (!r.ok) r = LifLoader::Load(path);  // a LIF, a plain image, or an MPO that is not a pair
	if (!r.ok) {
		LOGE("image failed to decode: %s", path.c_str());
		return false;
	}
	out.convergenceBaked = r.stereo && r.hasConvergence;
	if (out.convergenceBaked) {
		bake_lif_convergence(r.image, r.convergence);
	}
	// Layered resolution (desktop App.cpp parity): a composed LIF/MPO is
	// container metadata; a plain JPEG/PNG falls through to the filename, then
	// the content detector on the decoded pixels, then aspect.
	MediaInfo meta;
	meta.kind = MediaKind::Image;
	meta.layout = r.layout;
	out.info = resolve(name, MediaKind::Image, r.image, r.stereo ? &meta : nullptr);
	out.image = std::move(r.image);
	return true;
}

void
StillImage::ResolveFrame(const std::string &name, StillImage &still)
{
	still.info = resolve(name, MediaKind::Video, still.image, nullptr);
	still.convergenceBaked = false;
}

}  // namespace mp
