// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// NeighbourStills: stills of the files either side of the one playing, on the GPU before a
// swipe needs them, so the next file shows while it slides in (#160). An image's still is
// the image as the player decodes it (StillImage: a LIF or MPO in 3D), no smaller than the
// screen shows it, so a swipe can make it the picture without decoding the file again. A
// video's still is its first frame (a two-track video's first track only, so flat). Render
// thread, with one low-priority worker thread that stages and decodes.
#pragma once

#include "browser/AndroidMediaProvider.h"
#include "still_image.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct SbsRenderer;

class NeighbourStills {
public:
	// What a swipe's open can do with the neighbour's still.
	enum class Use {
		Open,   // nothing: open the file
		Wait,   // an image's still is on its way
		Adopt,  // an image's still is ready to be the picture
	};
	// An adopted still, for the player's state.
	struct Adopted {
		mp::MediaInfo info;
		int width = 0, height = 0;
		std::string name;
		bool convergenceBaked = false;
	};

	NeighbourStills() = default;
	~NeighbourStills();

	NeighbourStills(const NeighbourStills &) = delete;
	NeighbourStills &operator=(const NeighbourStills &) = delete;

	// `stagingPath` is a file the worker may write. `screenShort` is the shorter side of a
	// view on screen, which a still keeps; `maxSide` bounds a still's sides.
	bool Initialize(mp::browser::AndroidMediaProvider *provider, SbsRenderer *renderer,
	                const std::string &stagingPath, uint32_t screenShort, uint32_t maxSide);
	void Shutdown();

	// The files either side of the one playing, -1 for none. Asks for the stills not made.
	void Want(int64_t previous, int64_t next);
	// Once a loop, outside a frame's recording: start what arrived, upload what is done.
	void Update();
	// Show the still of the neighbour `step` (+1 next, -1 previous) as the renderer's second
	// page at `offset`; no second page when `step` is 0 or its still is not ready.
	void ShowPage(int step, float offset);

	Use ForOpen(int step) const;
	// Make the image still of the neighbour `step` the renderer's picture (Use::Adopt). The
	// picture it replaces, `current`, becomes the still on the other side when it is an
	// image (`currentId` >= 0), so a swipe back has nothing to decode.
	bool Adopt(int step, Adopted &out, int64_t currentId, const Adopted &current);

private:
	struct Slot {
		int64_t id = -1;
		bool image = false;   // its source is an image file (known once it arrives)
		bool failed = false;  // no still could be made
		bool ready = false;   // uploaded
		Adopted shown;
	};
	struct Done {
		int64_t id = 0;
		bool ok = false;
		std::string name;
		mp::StillImage still;
	};

	Slot *SlotFor(int64_t id);
	void Work();

	mp::browser::AndroidMediaProvider *provider_ = nullptr;
	SbsRenderer *renderer_ = nullptr;
	std::string stagingPath_;
	uint32_t screenShort_ = 0, maxSide_ = 0;
	Slot slots_[2];  // the previous and the next file: the renderer's still slots 0 and 1

	std::thread worker_;
	std::mutex mutex_;  // guards the rest
	std::condition_variable wake_;
	bool stop_ = false;
	std::deque<mp::browser::StillSource> jobs_;
	std::deque<Done> done_;
};
