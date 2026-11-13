// SPDX-License-Identifier: Apache-2.0
//
// PairDiskCache: the 3D grid's generated stereo pairs (StereoPairs.h) kept on disk, so a
// pair is made once per file version, not on every launch or scroll back (#180). Each pair
// is the whole side-by-side thumbnail as a JPEG (quality 85) under the app's cache dir
// (Android: Context.getCacheDir()/stereo_thumbs, which the OS may clear; never external
// storage), named by Key(): the media id (the content URI's), its modified time and size,
// the thumbnail size and view scale the eyes are made from, and kPairFormatVersion.
//
// Bounded (kMaxBytes): least recently used first, by file mtime (a hit touches it), trimmed
// after each store. A write goes to a temp file and is renamed, so a reader never sees a
// partial pair; a file that does not decode is removed by the caller and made again.
//
// Not thread-safe: StereoPairs' worker owns it, so no disk I/O reaches the render thread.
// The system's plain thumbnails are not kept here (MediaStore caches those).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp::browser {

class PairDiskCache {
public:
    // Bump whenever a pair would come out different: the pairing, the warp, the parallax
    // model, the eye sizes, the encoding. Old files then miss and age out.
    static constexpr uint32_t kPairFormatVersion = 1;
    static constexpr uint64_t kMaxBytes = 96ull << 20;

    // The file name stem of a pair: 16 hex digits of a hash of everything that shapes it.
    static std::string Key(int64_t id, int64_t modified, uint64_t fileSize, uint32_t thumbW, uint32_t thumbH,
                           float scale, uint32_t version = kPairFormatVersion);

    PairDiskCache() = default;
    ~PairDiskCache() { Shutdown(); }
    PairDiskCache(const PairDiskCache&) = delete;
    PairDiskCache& operator=(const PairDiskCache&) = delete;

    // Create `dir` if needed, drop stray temp files, index what is there, trim to `maxBytes`.
    bool Initialize(const std::string& dir, uint64_t maxBytes = kMaxBytes);
    void Shutdown();
    bool Ready() const { return !dir_.empty(); }

    // The stored JPEG of `key`, read whole; marks it recently used.
    bool Load(const std::string& key, std::vector<uint8_t>& jpeg);
    // Store w x h RGBA8 as `key` (temp + rename), then trim.
    bool Store(const std::string& key, const uint8_t* rgba, uint32_t w, uint32_t h);
    // A stored pair that did not decode: delete it.
    void Remove(const std::string& key);

    uint64_t Bytes() const { return bytes_; }
    size_t Count() const { return entries_.size(); }
    uint64_t MaxBytes() const { return maxBytes_; }

private:
    struct Entry {
        uint64_t bytes = 0;
        int64_t tick = 0;  // last use: the file's mtime, in the file clock's units
    };
    std::string PathOf(const std::string& key) const;
    int64_t Now();
    void Trim();

    std::string dir_;
    uint64_t maxBytes_ = kMaxBytes;
    std::unordered_map<std::string, Entry> entries_;
    uint64_t bytes_ = 0;
    int64_t lastTick_ = 0;
};

} // namespace mp::browser
