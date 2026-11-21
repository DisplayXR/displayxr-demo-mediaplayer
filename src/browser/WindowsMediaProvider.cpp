// SPDX-License-Identifier: Apache-2.0
#include "browser/WindowsMediaProvider.h"

#include "Log.h"
#include "browser/SequenceTiles.h"
#include "browser/WinShellMedia.h"
#include "media/LifLoader.h"
#include "media/MediaSource.h"
#include "media/MpoLoader.h"
#include "media/VideoThumbnail.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <iterator>

namespace mp::browser {

namespace {

// The sidebar, in order. Each source holds the items with its bit.
enum Bit { kRecents = 0, kPictures, kVideos, kDownloads, kIndex, kPinned, kThisFolder };

constexpr int kNewest = 50;              // Media folders' Newest tile

// What the player opens, and whether it is a video, by extension alone (quietly:
// MediaSource::Identify logs each call, and a walk sees thousands of files).
bool Accept(const std::string& path, bool& video) {
    if (!MediaSource::IsSupported(path)) return false;
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string() : path.substr(dot + 1);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    static const char* const kVideo[] = {"mp4", "mkv", "mov", "m4v", "webm", "3gp", "mxf"};
    video = std::find(std::begin(kVideo), std::end(kVideo), ext) != std::end(kVideo);
    return true;
}

// '/'-separated, with a trailing '/' for a folder.
std::string Slashes(std::string p, bool folder) {
    std::replace(p.begin(), p.end(), '\\', '/');
    if (folder && !p.empty() && p.back() != '/') p += '/';
    return p;
}

std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// A file named in the Recent list, from its directory entry only. False if it is gone,
// a folder, or not something the player opens.
bool StatFile(const std::string& path, win::FoundFile& f) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(Wide(path).c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (!Accept(path, f.video)) return false;
    f.path = path;
    const size_t cut = path.find_last_of("\\/");
    f.folder = cut == std::string::npos ? std::string() : path.substr(0, cut + 1);
    f.name = cut == std::string::npos ? path : path.substr(cut + 1);
    f.bytes = ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    const uint64_t t = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    f.dateMs = t ? (int64_t)(t / 10000ULL) - 11644473600000LL : 0;
    f.modified = f.dateMs / 1000;
    f.cloudOnly = win::IsCloudOnly(d.dwFileAttributes);
    return true;
}

} // namespace

int64_t PathId(const std::string& path) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : path) {
        if (c == '\\') c = '/';
        h ^= (uint64_t)std::tolower(c);
        h *= 1099511628211ULL;
    }
    return (int64_t)(h & ((1ULL << 62) - 1));
}

WindowsMediaProvider::WindowsMediaProvider() {
    listThread_ = std::thread([this] { ListWorker(); });
    thumbThread_ = std::thread([this] { ThumbWorker(); });
}

WindowsMediaProvider::~WindowsMediaProvider() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    listWake_.notify_all();
    thumbWake_.notify_all();
    if (listThread_.joinable()) listThread_.join();
    if (thumbThread_.joinable()) thumbThread_.join();
}

void WindowsMediaProvider::SetRecents(std::vector<std::string> paths) {
    std::lock_guard<std::mutex> lock(mutex_);
    recents_ = std::move(paths);
}

void WindowsMediaProvider::SetPinned(std::vector<std::string> folders) {
    std::lock_guard<std::mutex> lock(mutex_);
    pinned_ = std::move(folders);
}

void WindowsMediaProvider::SetHidden(std::vector<std::string> folders) {
    std::lock_guard<std::mutex> lock(mutex_);
    hidden_ = std::move(folders);
}

void WindowsMediaProvider::SetSources(bool libraries, bool index) {
    std::lock_guard<std::mutex> lock(mutex_);
    libraries_ = libraries;
    index_ = index;
}

void WindowsMediaProvider::SetThisFolder(std::string folder) {
    std::lock_guard<std::mutex> lock(mutex_);
    thisFolder_ = std::move(folder);
}

void WindowsMediaProvider::SetWalkLimits(int depth, double budgetMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    walkDepth_ = depth;
    walkBudgetMs_ = budgetMs;
}

void WindowsMediaProvider::SetCollapseSequences(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    collapseSequences_ = on;
}

std::string WindowsMediaProvider::PathOf(int64_t id) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    const auto it = paths_.find(id);
    return it == paths_.end() ? std::string() : it->second;
}

bool WindowsMediaProvider::TakeOpen(std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!openPending_) return false;
    openPending_ = false;
    path = open_;
    return true;
}

void WindowsMediaProvider::Refresh() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refresh_ = true;
    }
    listWake_.notify_all();
}

bool WindowsMediaProvider::TakeSources(std::vector<Source>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!haveList_) return false;
    out = sources_;  // TakeItems() follows in the same frame and clears haveList_
    return true;
}

bool WindowsMediaProvider::TakeItems(std::vector<MediaItem>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!haveList_) return false;
    haveList_ = false;
    out = items_;
    return true;
}

void WindowsMediaProvider::WantThumbnails(const std::vector<ThumbnailRequest>& wanted) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wanted_ = wanted;
    }
    thumbWake_.notify_all();
}

bool WindowsMediaProvider::TakeThumbnail(Thumbnail& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_.empty()) return false;
    out = std::move(done_.front());
    done_.erase(done_.begin());
    return true;
}

void WindowsMediaProvider::Open(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = paths_.find(id);
    if (it == paths_.end()) return;
    open_ = it->second;
    openPending_ = true;
}

std::string WindowsMediaProvider::Notice() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    return notice_;
}

void WindowsMediaProvider::ListWorker() {
    win::ScopedCom com;
    for (;;) {
        std::vector<std::string> recents, pinned, hidden;
        bool libraries = true, useIndex = true;
        std::string thisFolder;
        int walkDepth = 6;
        double walkBudgetMs = 3000.0;
        bool collapse = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            listWake_.wait(lock, [this] { return stop_ || refresh_; });
            if (stop_) return;
            refresh_ = false;
            recents = recents_;
            pinned = pinned_;
            libraries = libraries_;
            useIndex = index_;
            thisFolder = thisFolder_;
            walkDepth = walkDepth_;
            walkBudgetMs = walkBudgetMs_;
            collapse = collapseSequences_;
            for (const std::string& h : hidden_) {
                std::string k = Slashes(h, true);
                for (char& c : k) c = (char)std::tolower((unsigned char)c);
                hidden.push_back(std::move(k));
            }
        }
        const double t0 = win::NowMs();

        struct Entry {
            win::FoundFile file;
            uint64_t bits = 0;
            int order = 0;
        };
        std::unordered_map<int64_t, Entry> all;
        auto add = [&](win::FoundFile f, int bit, int order) {
            const int64_t id = PathId(f.path);
            auto it = all.find(id);
            if (it == all.end()) {
                Entry e;
                e.file = std::move(f);
                e.bits = 1ull << bit;
                e.order = order;
                all.emplace(id, std::move(e));
                return;
            }
            Entry& e = it->second;
            e.bits |= 1ull << bit;
            if (bit == kRecents) e.order = order;
            // The index knows sizes and the date taken; a folder walk does not.
            if (!e.file.width && f.width) e.file.width = f.width, e.file.height = f.height;
            if (!e.file.durationMs) e.file.durationMs = f.durationMs;
            if (f.dateMs && f.dateMs != f.modified * 1000) e.file.dateMs = f.dateMs;
        };
        auto walk = [&](const std::string& folder, int bit) {
            std::vector<win::FoundFile> found;
            const bool complete = win::WalkFolder(folder, walkDepth, win::NowMs() + walkBudgetMs, Accept, found);
            if (!complete) LOG_WARN("browser: %s - walk cut short after %.0f ms", folder.c_str(), walkBudgetMs);
            for (win::FoundFile& f : found) add(std::move(f), bit, 0);
        };

        int order = 0;
        for (const std::string& p : recents) {
            win::FoundFile f;
            if (StatFile(p, f)) add(std::move(f), kRecents, order++);
        }
        // Settings can leave out the library walks and the index (#265): no source, no walk.
        const std::vector<std::string> pictures =
            libraries ? win::LibraryFolders(win::Library::Pictures) : std::vector<std::string>();
        const std::vector<std::string> videos =
            libraries ? win::LibraryFolders(win::Library::Videos) : std::vector<std::string>();
        const std::string downloads = libraries ? win::DownloadsFolder() : std::string();
        for (const std::string& f : pictures) walk(f, kPictures);
        for (const std::string& f : videos) walk(f, kVideos);
        if (!downloads.empty()) walk(downloads, kDownloads);
        for (const std::string& f : pinned) walk(f, kPinned);
        if (!thisFolder.empty()) walk(thisFolder, kThisFolder);
        std::vector<win::FoundFile> indexed;
        std::string error;
        win::IndexSkipped skipped;
        const bool indexOk = useIndex && win::QueryIndex(win::IndexQuery{}, indexed, error, &skipped);
        if (indexOk)
            LOG_INFO("browser: index left out software files - %d in code checkouts, %d in game libraries, "
                     "%d in asset folders, %d pictures beside a program",
                     skipped.code, skipped.library, skipped.assetFolder, skipped.appFolder);
        auto isHidden = [&](const std::string& folder) {
            std::string k = Slashes(folder, true);
            for (char& c : k) c = (char)std::tolower((unsigned char)c);
            for (const std::string& h : hidden)
                if (k.compare(0, h.size(), h) == 0) return true;
            return false;
        };
        for (win::FoundFile& f : indexed)
            if (MediaSource::IsSupported(f.path) && !isHidden(f.folder)) add(std::move(f), kIndex, 0);

        // The sources: a tree source with one root is browsed from inside it.
        auto tree = [](Source& s, const std::vector<std::string>& roots) {
            s.mode = SourceMode::Tree;
            for (const std::string& r : roots) s.roots.push_back(Slashes(r, true));
            if (s.roots.size() == 1) {
                s.folder = s.roots.front();
                s.roots.clear();
            }
        };
        std::vector<Source> sources;
        auto source = [&](SourceKind kind, const char* name, int bit) -> Source& {
            Source s;
            s.kind = kind;
            s.name = name;
            s.bit = bit;
            sources.push_back(std::move(s));
            return sources.back();
        };
        source(SourceKind::Recents, "Recents", kRecents).byOrder = true;
        if (!thisFolder.empty()) tree(source(SourceKind::ThisFolder, "This folder", kThisFolder), {thisFolder});
        if (libraries) {
            tree(source(SourceKind::Pictures, "Pictures", kPictures), pictures);
            tree(source(SourceKind::Movies, "Videos", kVideos), videos);
        }
        if (!downloads.empty()) tree(source(SourceKind::Downloads, "Downloads", kDownloads), {downloads});
        if (useIndex) {
            Source& folders = source(SourceKind::Folders, "Media folders", kIndex);
            folders.mode = SourceMode::Grouped;
            folders.newest = kNewest;
        }
        if (!pinned.empty()) {
            // Pinned lists its folders first even when there is one, so each can be unpinned.
            Source& s = source(SourceKind::Pinned, "Pinned", kPinned);
            s.mode = SourceMode::Tree;
            for (const std::string& r : pinned) s.roots.push_back(Slashes(r, true));
        }

        // Pro (#267): a render's frames as one item per sequence, kept on its lowest frame
        // (which opens the sequence), with every frame's sources, the newest Recent place and
        // the frames' total size.
        std::unordered_map<int64_t, std::string> seqNames;
        if (collapse) {
            std::vector<int64_t> ids;
            std::vector<std::string> framePaths;
            for (const auto& kv : all) {
                ids.push_back(kv.first);
                framePaths.push_back(kv.second.file.path);
            }
            size_t folded = 0;
            for (const SequenceTile& t : GroupSequences(framePaths)) {
                Entry& keep = all[ids[t.first]];
                for (size_t m : t.members) {
                    if (m == t.first) continue;
                    const Entry& e = all[ids[m]];
                    if ((e.bits & (1ull << kRecents)) && (!(keep.bits & (1ull << kRecents)) || e.order < keep.order))
                        keep.order = e.order;
                    keep.bits |= e.bits;
                    keep.file.bytes += e.file.bytes;  // the size shown is the whole sequence's
                    all.erase(ids[m]);
                    ++folded;
                }
                seqNames[ids[t.first]] = SequenceTileName(keep.file.name, t.firstFrame, t.lastFrame);
            }
            if (folded) LOG_INFO("browser: %zu sequence(s), %zu frames folded into them", seqNames.size(), folded);
        }

        std::vector<MediaItem> items;
        std::unordered_map<int64_t, std::string> paths;
        std::unordered_map<int64_t, bool> cloud;
        items.reserve(all.size());
        int nCloud = 0;
        for (auto& kv : all) {
            const win::FoundFile& f = kv.second.file;
            MediaItem m;
            m.id = kv.first;
            const auto seq = seqNames.find(kv.first);
            m.name = seq != seqNames.end() ? seq->second : f.name;
            m.type = f.video ? MediaType::Video : MediaType::Image;
            m.dateMs = f.dateMs;
            m.modified = f.modified;
            m.bytes = f.bytes;
            m.durationMs = f.durationMs;
            m.folder = Slashes(f.folder, true);
            m.width = f.width;
            m.height = f.height;
            m.sources = kv.second.bits;
            m.order = kv.second.order;
            m.cloudOnly = f.cloudOnly;
            nCloud += f.cloudOnly;
            paths[m.id] = f.path;
            cloud[m.id] = f.cloudOnly;
            items.push_back(std::move(m));
        }
        LOG_INFO("browser: %zu items (%d cloud-only), index %s (%zu), libraries %s, %.0f ms", items.size(), nCloud,
                 !useIndex ? "not used" : indexOk ? "ok" : "OFF", indexed.size(), libraries ? "on" : "off",
                 win::NowMs() - t0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sources_ = std::move(sources);
            items_ = std::move(items);
            paths_ = std::move(paths);
            cloud_ = std::move(cloud);
            notice_ = indexOk || !useIndex ? std::string()
                              : "Windows Search is off, so Media folders is empty. Pictures, Videos, "
                                "Downloads and pinned folders still work.";
            if (!indexOk && useIndex) LOG_WARN("browser: index query failed: %s", error.c_str());
            haveList_ = true;
        }
    }
}

void WindowsMediaProvider::ThumbWorker() {
    win::ScopedCom com;
    for (;;) {
        ThumbnailRequest req;
        std::string path;
        bool cloudOnly = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            thumbWake_.wait(lock, [this] { return stop_ || !wanted_.empty(); });
            if (stop_) return;
            req = wanted_.front();
            wanted_.erase(wanted_.begin());
            const auto it = paths_.find(req.id);
            if (it != paths_.end()) path = it->second;
            const auto c = cloud_.find(req.id);
            cloudOnly = c != cloud_.end() && c->second;
        }
        Thumbnail t;
        t.id = req.id;
        // A cloud placeholder: only what the thumbnail cache has, so it is never downloaded.
        if (!path.empty())
            win::ShellThumbnail(path, req.maxEdge ? req.maxEdge : 512, cloudOnly, t.width, t.height, t.rgba);
        // Windows has none for many local videos (HEVC without its codec pack, MKV, large
        // files): decode a frame ourselves. Never for a cloud file, which this would download.
        bool video = false;
        const bool known = !path.empty() && Accept(path, video);
        if (t.width == 0 && known && video && !cloudOnly)
            VideoThumbnail(path, req.maxEdge ? req.maxEdge : 512, t.width, t.height, t.rgba);
        // Is the file itself 3D (a LIF, an MPO)? Only a local still: a cloud file would be
        // downloaded to answer, and a two-track video needs a demuxer (both left 2D, marked
        // not checked by the classifier).
        if (req.checkFile && !cloudOnly && known && !video) {
            t.fileChecked = true;
            t.file3D = LifLoader::IsLif(path) || MpoLoader::IsMpo(path);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(t));
    }
}

} // namespace mp::browser
