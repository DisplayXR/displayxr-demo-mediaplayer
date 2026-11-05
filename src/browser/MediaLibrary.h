// SPDX-License-Identifier: Apache-2.0
//
// MediaLibrary: the media list the browser shows. It groups the items into sources (All
// media, Camera, Downloads, Pictures, Movies, Unsupported) and answers what a source holds
// under the view options. Pure logic, render thread.
#pragma once

#include "browser/MediaItem.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp::browser {

enum class SourceKind { All, Camera, Downloads, Pictures, Movies, Unsupported };

struct Source {
    SourceKind kind = SourceKind::All;
    std::string name;
    int count = 0;
};

enum class TypeFilter { All, Videos, Images };
enum class SortOrder { NewestFirst, OldestFirst };

struct ViewOptions {
    TypeFilter type = TypeFilter::All;
    SortOrder order = SortOrder::NewestFirst;
};

class MediaLibrary {
public:
    void SetItems(std::vector<MediaItem> items);

    const std::vector<MediaItem>& Items() const { return items_; }
    // The item with `id`, or null.
    const MediaItem* Find(int64_t id) const;
    // One per SourceKind, in that order. Unsupported holds what the player cannot open
    // (MediaSource::IsSupported); All media holds the rest. Camera and Downloads are
    // those folders with their subfolders, Pictures every other image, Movies every other
    // video.
    const std::vector<Source>& Sources() const { return sources_; }
    // The source besides All media that holds Items()[item].
    SourceKind Kind(size_t item) const { return kinds_[item]; }
    // Indices into Items() of what `source` holds under `options`, in display order.
    std::vector<size_t> Query(const Source& source, const ViewOptions& options) const;

private:
    std::vector<MediaItem> items_;
    std::vector<SourceKind> kinds_;             // per item
    std::unordered_map<int64_t, size_t> byId_;  // id -> index into items_
    std::vector<Source> sources_;
};

} // namespace mp::browser
