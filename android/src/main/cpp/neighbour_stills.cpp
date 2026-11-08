// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0

#include "neighbour_stills.h"

#include "sbs_renderer.h"

#include <android/log.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <utility>
#include <vector>

#define LOG_TAG "mediaplayer_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

// One eye's display aspect: only full SBS packs two eyes across one frame's width (as
// main.cpp's apply_layout).
float
eye_aspect(mp::StereoLayout layout, int w, int h)
{
	return (layout == mp::StereoLayout::SbsFull ? (float)w * 0.5f : (float)w) / (float)h;
}

// Box-filter `img` down by the largest whole factor that keeps it as sharp as the screen
// shows it: the picture is fitted so its shorter side fills `screenShort`. Then by more if
// a side is over `maxSide`.
void
shrink(mp::DecodedImage &img, mp::StereoLayout layout, uint32_t screenShort, uint32_t maxSide)
{
	const float aspect = eye_aspect(layout, img.width, img.height);
	// The eye's source pixels per screen pixel, across and down.
	const int eyeW = layout == mp::StereoLayout::Mono ? img.width : img.width / 2;
	const float shownW = aspect >= 1.0f ? (float)screenShort * aspect : (float)screenShort;
	const float shownH = aspect >= 1.0f ? (float)screenShort : (float)screenShort / aspect;
	int k = (int)std::min((float)eyeW / shownW, (float)img.height / shownH);
	k = std::max({k, 1, (img.width + (int)maxSide - 1) / (int)maxSide, (img.height + (int)maxSide - 1) / (int)maxSide});
	if (k <= 1) return;
	const int w = img.width / k, h = img.height / k;
	std::vector<uint8_t> out((size_t)w * h * 4);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			uint32_t sum[4] = {};
			for (int dy = 0; dy < k; ++dy) {
				const uint8_t *src = img.pixels.data() + ((size_t)(y * k + dy) * img.width + (size_t)x * k) * 4;
				for (int dx = 0; dx < k * 4; ++dx) sum[dx & 3] += src[dx];
			}
			uint8_t *dst = out.data() + ((size_t)y * w + x) * 4;
			for (int c = 0; c < 4; ++c) dst[c] = (uint8_t)(sum[c] / (uint32_t)(k * k));
		}
	}
	img.pixels.swap(out);
	img.width = w;
	img.height = h;
}

}  // namespace

NeighbourStills::~NeighbourStills() { Shutdown(); }

bool
NeighbourStills::Initialize(mp::browser::AndroidMediaProvider *provider, SbsRenderer *renderer,
                            const std::string &stagingPath, uint32_t screenShort, uint32_t maxSide)
{
	if (!provider || !renderer || screenShort == 0 || maxSide == 0) return false;
	provider_ = provider;
	renderer_ = renderer;
	stagingPath_ = stagingPath;
	screenShort_ = screenShort;
	maxSide_ = maxSide;
	stop_ = false;
	worker_ = std::thread(&NeighbourStills::Work, this);
	return true;
}

void
NeighbourStills::Shutdown()
{
	if (!worker_.joinable()) return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	worker_.join();
	for (const mp::browser::StillSource &s : jobs_) {
		if (s.fd >= 0) close(s.fd);
	}
	jobs_.clear();
	done_.clear();
	provider_ = nullptr;
	renderer_ = nullptr;
}

void
NeighbourStills::Want(int64_t previous, int64_t next)
{
	if (!provider_) return;
	const int64_t ids[2] = {previous, next};
	bool changed = false;
	for (int i = 0; i < 2; ++i) {
		if (slots_[i].id == ids[i]) continue;
		slots_[i] = Slot{};
		slots_[i].id = ids[i];
		changed = true;
	}
	if (!changed) return;
	std::vector<int64_t> ask;
	for (const Slot &s : slots_) {
		if (s.id >= 0 && !s.ready) ask.push_back(s.id);
	}
	provider_->WantStills(ask, maxSide_);
	// Work queued for files that are no longer either side is dropped.
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = jobs_.begin(); it != jobs_.end();) {
		if (SlotFor(it->id)) {
			++it;
			continue;
		}
		if (it->fd >= 0) close(it->fd);
		it = jobs_.erase(it);
	}
}

void
NeighbourStills::Update()
{
	if (!provider_) return;
	mp::browser::StillSource source;
	while (provider_->TakeStill(source)) {
		Slot *s = SlotFor(source.id);
		if (!s) {
			if (source.fd >= 0) close(source.fd);
			continue;
		}
		s->image = source.fd >= 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			jobs_.push_back(std::move(source));
		}
		wake_.notify_one();
	}

	std::deque<Done> done;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		done.swap(done_);
	}
	for (Done &d : done) {
		Slot *s = SlotFor(d.id);
		if (!s) continue;
		if (!d.ok) {
			s->failed = true;
			continue;
		}
		const int i = (int)(s - slots_);
		const mp::DecodedImage &img = d.still.image;
		renderer_->uploadStill(i, img.pixels.data(), (uint32_t)img.width, (uint32_t)img.height);
		s->ready = true;
		s->shown = Adopted{d.still.info, img.width, img.height, d.name, d.still.convergenceBaked};
		LOGI("still ready: %s %dx%d layout=%s", i ? "next" : "previous", img.width, img.height,
		     mp::LayoutName(d.still.info.layout));
	}
}

void
NeighbourStills::ShowPage(int step, float offset)
{
	if (!renderer_) return;
	const int i = step > 0 ? 1 : 0;
	if (step == 0 || !slots_[i].ready) {
		renderer_->setStillPage(-1, 0.0f, 1.0f, true);
		return;
	}
	const Adopted &a = slots_[i].shown;
	renderer_->setStillPage(i, offset, eye_aspect(a.info.layout, a.width, a.height),
	                        a.info.layout == mp::StereoLayout::Mono);
}

NeighbourStills::Use
NeighbourStills::ForOpen(int step) const
{
	if (step == 0) return Use::Open;
	const Slot &s = slots_[step > 0 ? 1 : 0];
	if (!s.image || s.failed) return Use::Open;
	return s.ready ? Use::Adopt : Use::Wait;
}

bool
NeighbourStills::Adopt(int step, Adopted &out, int64_t currentId, const Adopted &current)
{
	const int i = step > 0 ? 1 : 0;
	if (step == 0 || !slots_[i].image || !slots_[i].ready) return false;
	renderer_->adoptStill(i);
	out = slots_[i].shown;
	slots_[i] = Slot{};  // the slot holds the old picture's plane now
	if (currentId < 0) return true;
	renderer_->swapStills();
	Slot &kept = slots_[1 - i];
	kept = Slot{};
	kept.id = currentId;
	kept.image = kept.ready = true;
	kept.shown = current;
	return true;
}

NeighbourStills::Slot *
NeighbourStills::SlotFor(int64_t id)
{
	for (Slot &s : slots_) {
		if (s.id == id && id >= 0 && !s.ready && !s.failed) return &s;
	}
	return nullptr;
}

void
NeighbourStills::Work()
{
	// Background priority: decoding the neighbours must never take time from the frame.
	setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
	for (;;) {
		mp::browser::StillSource source;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
			if (stop_) return;
			source = std::move(jobs_.front());
			jobs_.pop_front();
		}
		Done d;
		d.id = source.id;
		d.name = source.name;
		if (source.fd >= 0) {
			const off_t length = lseek(source.fd, 0, SEEK_END);
			d.ok = length > 0 && mp::StillImage::Stage(source.fd, 0, (long long)length, stagingPath_) &&
			       mp::StillImage::Decode(stagingPath_, source.name, d.still);
			close(source.fd);
		} else if (!source.rgba.empty()) {
			d.still.image.pixels = std::move(source.rgba);
			d.still.image.width = (int)source.width;
			d.still.image.height = (int)source.height;
			mp::StillImage::ResolveFrame(source.name, d.still);
			d.ok = true;
		}
		if (d.ok) shrink(d.still.image, d.still.info.layout, screenShort_, maxSide_);
		std::lock_guard<std::mutex> lock(mutex_);
		done_.push_back(std::move(d));
	}
}
