// SPDX-License-Identifier: Apache-2.0
//
// WindowsMediaProvider: the desktop browser's media on Windows (#252). Sources:
// Recents, This folder (Pro, #267), Pictures and Videos (the libraries), Downloads, Media
// folders (the Search index, grouped by folder) and Pinned folders; the app picks which. Lists are made on a worker thread and
// thumbnails on another, both through WinShellMedia: nothing reads a cloud-only file's
// bytes. Render-thread calls never block.
#pragma once

#include "browser/MediaProvider.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mp::browser {

class WindowsMediaProvider final : public MediaProvider {
public:
    WindowsMediaProvider();
    ~WindowsMediaProvider() override;

    WindowsMediaProvider(const WindowsMediaProvider&) = delete;
    WindowsMediaProvider& operator=(const WindowsMediaProvider&) = delete;

    // The Recent list (files and folders, newest first) and the pinned folders, as the app
    // keeps them. Used by the next Refresh().
    void SetRecents(std::vector<std::string> paths);
    void SetPinned(std::vector<std::string> folders);
    // Folders left out of Media folders (with what is under them): "Hide from Media folders".
    void SetHidden(std::vector<std::string> folders);
    // Settings (#265): list Pictures / Videos / Downloads (the library walks) and Media
    // folders (the Search index). Both on by default. Used by the next Refresh().
    void SetSources(bool libraries, bool index);
    // Pro (#267): "This folder", a folder tree after Recents (empty = no such source); how
    // deep and how long a folder walk may go (slow network shares); and numbered EXR / DPX /
    // TIFF frames as one item per sequence (SequenceTiles.h). Used by the next Refresh().
    void SetThisFolder(std::string folder);
    void SetWalkLimits(int depth, double budgetMs);
    void SetCollapseSequences(bool on);

    // The path of an item, or empty.
    std::string PathOf(int64_t id) const;
    // The item Open() was last asked for, once.
    bool TakeOpen(std::string& path);

    MediaAccess Access() const override { return MediaAccess::Full; }
    void RequestAccess() override {}
    void Refresh() override;
    bool TakeSources(std::vector<Source>& out) override;
    bool TakeItems(std::vector<MediaItem>& out) override;
    void WantThumbnails(const std::vector<ThumbnailRequest>& wanted) override;
    bool TakeThumbnail(Thumbnail& out) override;
    void Open(int64_t id) override;
    bool CanDelete() const override { return false; }
    void Delete(const std::vector<int64_t>&) override {}
    std::string Notice() const override;

private:
    void ListWorker();
    void ThumbWorker();

    std::mutex mutex_;
    std::condition_variable listWake_, thumbWake_;
    bool stop_ = false;
    bool refresh_ = false;

    // Inputs (under mutex_).
    std::vector<std::string> recents_, pinned_, hidden_;
    bool libraries_ = true, index_ = true;
    std::string thisFolder_;
    int walkDepth_ = 6;
    double walkBudgetMs_ = 3000.0;  // per folder tree (libraries, Downloads, a pin)
    bool collapseSequences_ = false;

    // Results (under mutex_).
    bool haveList_ = false;
    std::vector<Source> sources_;
    std::vector<MediaItem> items_;
    std::string notice_;
    std::unordered_map<int64_t, std::string> paths_;  // id -> native path
    std::unordered_map<int64_t, bool> cloud_;         // id -> cloud-only

    std::vector<ThumbnailRequest> wanted_;
    std::vector<Thumbnail> done_;
    std::string open_;
    bool openPending_ = false;

    std::thread listThread_, thumbThread_;
};

// A stable id for a path (FNV-1a of the case-folded, '/'-separated path, kept to 62
// bits: the browser's right-eye keys are -id-2). Exposed for tests.
int64_t PathId(const std::string& path);

} // namespace mp::browser
