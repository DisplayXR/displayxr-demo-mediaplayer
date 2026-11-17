// SPDX-License-Identifier: Apache-2.0
//
// MediaLibrary: the media list the browser shows. It groups the items into sources and
// answers what a source, or one of its folders, holds under the view options. Pure logic,
// render thread.
//
// Two ways to get sources:
// - Built in (Android, #160): All media, Camera, Downloads, Pictures, Movies, Unsupported,
//   worked out from each item's folder.
// - Declared by the provider (desktop, #252): SetSources() with a list of sources, each
//   holding the items that carry its bit in MediaItem::sources. A source is browsed flat,
//   as a folder tree under one or more roots, or grouped by folder.
#pragma once

#include "browser/MediaItem.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp::browser {

// Built-in sources first (their values index the built-in table), then the desktop's.
enum class SourceKind { All, Camera, Downloads, Pictures, Movies, Unsupported, Recents, Folders, Pinned };

// How a declared source is browsed.
enum class SourceMode {
    Flat,     // one list
    Tree,     // folders under `folder` (one root) or under `roots` (several: listed first)
    Grouped,  // one tile per folder that holds something, no nesting; a "Newest" tile first
};

struct Source {
    SourceKind kind = SourceKind::All;
    std::string name;
    int count = 0;
    // Browsed as folders from here (Downloads: "Download/"), or empty: one flat list, or
    // (Tree with several roots, Grouped) the list of folders.
    std::string folder;
    // --- Declared sources (SetSources) ---
    int bit = -1;  // the MediaItem::sources bit it holds; -1 = a built-in source
    SourceMode mode = SourceMode::Flat;
    std::vector<std::string> roots;  // Tree with several roots: the folders listed first
    bool dateSections = false;       // grouped into "Today", "October 2026"...
    bool byOrder = false;            // in MediaItem::order, not by date (Recents)
    int newest = 0;                  // Grouped: the "Newest" tile's item count (0 = none)
};

struct Folder {
    std::string name;  // e.g. "ZTEPics"
    std::string path;  // e.g. "Download/MEDIA/ZTEPics/"
    int count = 0;     // what it and its sub-folders hold under the view options
    std::string detail;  // a second line, e.g. the parent folder of a grouped folder
};

enum class TypeFilter { All, Videos, Images };
enum class SortOrder { NewestFirst, OldestFirst };

struct ViewOptions {
    TypeFilter type = TypeFilter::All;
    SortOrder order = SortOrder::NewestFirst;
};

// A Grouped source's "Newest" tile: the folder path that stands for it.
extern const char* const kNewestFolder;

class MediaLibrary {
public:
    // Declare the sources (desktop). Empty: the built-in sources. Takes effect at the next
    // SetItems(). An unsupported item (MediaSource::IsSupported) only goes to the source of
    // kind Unsupported, if one is declared.
    void SetSources(std::vector<Source> sources) { declared_ = std::move(sources); }
    void SetItems(std::vector<MediaItem> items);

    const std::vector<MediaItem>& Items() const { return items_; }
    // The item with `id`, or null.
    const MediaItem* Find(int64_t id) const;
    // Built in: one per SourceKind up to Unsupported, in that order. Unsupported holds what
    // the player cannot open (MediaSource::IsSupported); All media holds the rest. Camera
    // and Downloads are those folders with their subfolders, Pictures every other image,
    // Movies every other video. Declared: the list given to SetSources, with counts.
    const std::vector<Source>& Sources() const { return sources_; }
    // Built in: the source besides All media that holds Items()[item].
    SourceKind Kind(size_t item) const { return kinds_[item]; }
    // Whether the player can open Items()[item].
    bool Supported(size_t item) const { return kinds_[item] != SourceKind::Unsupported; }
    // Indices into Items() of what `source` holds under `options`, in display order. With a
    // `folder`, only what is directly in it.
    std::vector<size_t> Query(const Source& source, const ViewOptions& options,
                              const std::string& folder = std::string()) const;
    // The sub-folders of `folder` that hold something of `source` under `options`, by name.
    std::vector<Folder> Folders(const Source& source, const ViewOptions& options, const std::string& folder) const;
    // The folder one up from `folder` in `source` (`source.folder` at the top).
    std::string Parent(const Source& source, const std::string& folder) const;
    // The parts of `folder` under the source, for the title: "MEDIA", "ZTEPics".
    std::vector<std::string> Breadcrumb(const Source& source, const std::string& folder) const;

private:
    bool Holds(const Source& source, const ViewOptions& options, size_t item) const;
    void Sort(const Source& source, const ViewOptions& options, std::vector<size_t>& out) const;

    std::vector<Source> declared_;
    std::vector<MediaItem> items_;
    std::vector<SourceKind> kinds_;             // per item: built in, its source; else Unsupported or All
    std::unordered_map<int64_t, size_t> byId_;  // id -> index into items_
    std::vector<Source> sources_;
};

} // namespace mp::browser
