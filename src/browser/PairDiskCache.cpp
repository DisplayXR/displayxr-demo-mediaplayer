// SPDX-License-Identifier: Apache-2.0
#include "browser/PairDiskCache.h"

// Static: other targets (the tests, the desktop renderer) carry their own copy.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace mp::browser {

namespace fs = std::filesystem;

namespace {

constexpr int kJpegQuality = 85;
constexpr const char* kExt = ".jpg";
constexpr const char* kTmpExt = ".tmp";

uint64_t Fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

void Append(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    out->insert(out->end(), (const uint8_t*)data, (const uint8_t*)data + size);
}

} // namespace

std::string PairDiskCache::Key(int64_t id, int64_t modified, uint64_t fileSize, uint32_t thumbW, uint32_t thumbH,
                               float scale, uint32_t version) {
    char text[160];
    std::snprintf(text, sizeof(text), "media:%lld|m%lld|s%llu|t%ux%u|v%ld|f%u", (long long)id, (long long)modified,
                  (unsigned long long)fileSize, thumbW, thumbH, std::lround((double)scale * 10000.0), version);
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)Fnv1a(text));
    return hex;
}

std::string PairDiskCache::PathOf(const std::string& key) const { return dir_ + "/" + key + kExt; }

int64_t PairDiskCache::Now() {
    // Strictly increasing within a session, so two uses in one clock tick still order.
    const int64_t now = (int64_t)fs::file_time_type::clock::now().time_since_epoch().count();
    lastTick_ = std::max(now, lastTick_ + 1);
    return lastTick_;
}

bool PairDiskCache::Initialize(const std::string& dir, uint64_t maxBytes) {
    Shutdown();
    if (dir.empty()) return false;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) return false;
    dir_ = dir;
    maxBytes_ = maxBytes;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (p.extension() != kExt) {  // a write cut short, or anything else: not ours to keep
            fs::remove(p, ec);
            continue;
        }
        Entry e;
        e.bytes = (uint64_t)it->file_size(ec);
        e.tick = (int64_t)it->last_write_time(ec).time_since_epoch().count();
        lastTick_ = std::max(lastTick_, e.tick);
        entries_[p.stem().string()] = e;
        bytes_ += e.bytes;
    }
    Trim();
    return true;
}

void PairDiskCache::Shutdown() {
    dir_.clear();
    entries_.clear();
    bytes_ = 0;
}

bool PairDiskCache::Load(const std::string& key, std::vector<uint8_t>& jpeg) {
    auto it = entries_.find(key);
    if (!Ready() || it == entries_.end()) return false;
    std::ifstream f(PathOf(key), std::ios::binary | std::ios::ate);
    const std::streamoff len = f ? (std::streamoff)f.tellg() : 0;
    if (len <= 0) {  // cleared under us (the OS trims cache dirs)
        bytes_ -= it->second.bytes;
        entries_.erase(it);
        return false;
    }
    jpeg.resize((size_t)len);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(jpeg.data()), (std::streamsize)len);
    if (!f) return false;
    it->second.tick = Now();
    std::error_code ec;
    fs::last_write_time(PathOf(key), fs::file_time_type(fs::file_time_type::duration(it->second.tick)), ec);
    return true;
}

bool PairDiskCache::Store(const std::string& key, const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (!Ready() || !rgba || w == 0 || h == 0) return false;
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        rgb[i * 3] = rgba[i * 4];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::vector<uint8_t> jpeg;
    if (!stbi_write_jpg_to_func(&Append, &jpeg, (int)w, (int)h, 3, rgb.data(), kJpegQuality) || jpeg.empty())
        return false;
    const std::string tmp = dir_ + "/" + key + kTmpExt;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(jpeg.data()), (std::streamsize)jpeg.size());
        if (!f) {
            f.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, PathOf(key), ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    Entry& e = entries_[key];
    bytes_ -= e.bytes;
    e.bytes = jpeg.size();
    e.tick = Now();
    bytes_ += e.bytes;
    fs::last_write_time(PathOf(key), fs::file_time_type(fs::file_time_type::duration(e.tick)), ec);
    Trim();
    return true;
}

void PairDiskCache::Remove(const std::string& key) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        bytes_ -= it->second.bytes;
        entries_.erase(it);
    }
    std::error_code ec;
    if (Ready()) fs::remove(PathOf(key), ec);
}

void PairDiskCache::Trim() {
    if (bytes_ <= maxBytes_) return;
    // Down to 90 %, so a full cache does not trim on every store.
    const uint64_t target = maxBytes_ - maxBytes_ / 10;
    std::vector<std::pair<int64_t, std::string>> order;
    order.reserve(entries_.size());
    for (const auto& kv : entries_) order.emplace_back(kv.second.tick, kv.first);
    std::sort(order.begin(), order.end());
    std::error_code ec;
    for (const auto& o : order) {
        if (bytes_ <= target) break;
        auto it = entries_.find(o.second);
        bytes_ -= it->second.bytes;
        entries_.erase(it);
        fs::remove(PathOf(o.second), ec);
    }
}

} // namespace mp::browser
