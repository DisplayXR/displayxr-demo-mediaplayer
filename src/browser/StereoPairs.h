// SPDX-License-Identifier: Apache-2.0
//
// StereoPairs: the 3D grid's pair for a 3D photo whose thumbnail is one view (#180). The
// system's thumbnail of a LIF or an MPO is its first (left) view only, so the grid would
// show it flat. This makes the right view at thumbnail size from the file itself:
//   - LIF (two views): LifLoader::FindViews reads the trailer and the JSON (a few KB) and
//     gives the right view's embedded JPEG; it is read and decoded SCALED to the eye size.
//     The LIFs seen so far carry no thumbnail or preview of their own (classic schema:
//     the base JPEG plus one view blob, no EXIF thumbnail in either), so a scaled decode is
//     the cheapest source.
//   - LIF (one view + a disparity map: what a 2D->3D conversion writes): LifLoader::
//     FindDepthView gives the image and the map; the map is decoded at the eye size and
//     both eyes come from DepthWarp::MakeSbs at the parallax the player uses for the file
//     (LifLoader::Load's one-view path), so the tile's depth matches the player's.
//   - MPO: MpoLoader::FindSecondView reads the MP index from the file's head and gives the
//     second view's JPEG, decoded scaled the same way.
// The left eye is the system's thumbnail when the left view is the file's base JPEG (it
// always is for an MPO, and for the LIFs seen so far) and the thumbnail is the whole view,
// else it is decoded too. A thumbnail of another aspect than the right view (a centre
// crop: the file's own square EXIF thumbnail) would otherwise put the whole right view, squeezed, next
// to a crop of the left: both eyes are then decoded at the view's own aspect.
//
// One low-priority worker thread, separate from the provider's thumbnail threads and the
// player's stills thread, so it never starves opening a file or the swipe's neighbours.
// Results are kept in memory, bounded, by id and modified time. Render thread API.
#pragma once

#include "browser/MediaProvider.h"
#include "media/LifLoader.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mp::browser {

// Decode a JPEG (or a PNG: a one-view LIF's disparity map) to exactly w x h RGBA8,
// unpremultiplied, scaling while decoding (never at full size).
// Called on the worker thread. False if it cannot.
using ScaledJpegDecoder =
    std::function<bool(const uint8_t* data, size_t size, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba)>;

// Read `size` bytes at `offset` of the open file `fd`. False on a short read.
using FileReader = std::function<bool(int fd, uint64_t offset, void* dst, size_t size)>;

// The eye size for a w x h one-view thumbnail at `scale` (the 3D mode's view scale, <= 1):
// at least 16 px on each side, never larger than the thumbnail.
void PairEyeSize(uint32_t w, uint32_t h, float scale, uint32_t& ew, uint32_t& eh);

// The w x h of a baseline or progressive JPEG, from its SOF header. False if there is none.
bool JpegSize(const uint8_t* data, size_t size, uint32_t& w, uint32_t& h);

// Whether a w x h one-view thumbnail is the whole of a viewW x viewH view (the same aspect,
// to rounding). Not every file's is: the NP02J camera's LIFs (since 2026-07) carry a
// 512 x 512 EXIF thumbnail, a centre crop of the 16:10 view, and MediaStore hands that back.
// True when the view's size is unknown (0).
bool ThumbnailIsWholeView(uint32_t w, uint32_t h, uint32_t viewW, uint32_t viewH);

// The eye size when the thumbnail is not the whole view: the view's own aspect, just
// covering PairEyeSize(w, h, scale), and never larger than the view.
void PairEyeSizeOfView(uint32_t w, uint32_t h, uint32_t viewW, uint32_t viewH, float scale, uint32_t& ew,
                       uint32_t& eh);

// Resize w x h RGBA8 to ew x eh with a box filter (a downscale; an upscale repeats pixels).
void ResizeRgba(const uint8_t* src, uint32_t w, uint32_t h, uint32_t ew, uint32_t eh, std::vector<uint8_t>& out);

// Put two ew x eh RGBA8 eyes side by side: a 2ew x eh full side-by-side image.
void ComposePair(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right, uint32_t ew, uint32_t eh,
                 std::vector<uint8_t>& out);

class StereoPairs {
public:
    StereoPairs() = default;
    ~StereoPairs();

    StereoPairs(const StereoPairs&) = delete;
    StereoPairs& operator=(const StereoPairs&) = delete;

    // `read` and `close` work on the descriptors Make() takes (tests pass fakes).
    bool Initialize(ScaledJpegDecoder decode, FileReader read, std::function<void(int)> close);
    void Shutdown();
    bool Ready() const { return worker_.joinable(); }

    // Make item `id`'s pair from its open file `fd` (taken: always closed) and its one-view
    // thumbnail (w x h RGBA8, copied). The eyes are made at PairEyeSize(w, h, scale).
    void Make(int64_t id, int64_t modified, int fd, uint64_t fileSize, const uint8_t* left, uint32_t w, uint32_t h,
              float scale);
    // Take the finished pairs (once a frame): a full side-by-side thumbnail each, cached.
    // `failed` gets the ids that have no pair (they stay flat).
    void TakeResults(std::vector<Thumbnail>& pairs, std::vector<int64_t>& failed);
    // The cached pair of `id` at `modified`, copied into `out`.
    bool Cached(int64_t id, int64_t modified, Thumbnail& out);
    bool Has(int64_t id, int64_t modified) const;
    // Queued or being made.
    bool Busy(int64_t id) const { return busy_.count(id) != 0; }

    // Memory the cache may hold.
    static constexpr size_t kCacheBytes = 24u << 20;
    static constexpr size_t kMaxQueued = 24;

private:
    struct Job {
        int64_t id = 0, modified = 0;
        int fd = -1;
        uint64_t size = 0;
        uint32_t w = 0, h = 0;
        float scale = 1.0f;
        std::vector<uint8_t> left;
    };
    struct Done {
        int64_t id = 0, modified = 0;
        bool ok = false;
        Thumbnail sbs;
    };
    struct Entry {
        int64_t modified = 0;
        Thumbnail sbs;
        std::list<int64_t>::iterator lru;
    };

    void Work();
    bool MakeOne(const Job& job, Thumbnail& out);
    bool MakeFromDepth(const Job& job, const LifLoader::DepthViewSpan& one, Thumbnail& out);
    void Keep(int64_t id, int64_t modified, const Thumbnail& sbs);

    ScaledJpegDecoder decode_;
    FileReader read_;
    std::function<void(int)> close_;

    std::unordered_set<int64_t> busy_;                 // render thread
    std::unordered_map<int64_t, Entry> cache_;         // render thread
    std::list<int64_t> lru_;                           // most recently used first
    size_t cacheBytes_ = 0;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Done> done_;
    bool stop_ = false;
    std::thread worker_;
    int statCount_ = 0, statOk_ = 0;  // worker: the cost log, every 4 pairs
    double statMs_ = 0.0;
};

} // namespace mp::browser
