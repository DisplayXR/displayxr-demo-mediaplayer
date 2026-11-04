// SPDX-License-Identifier: Apache-2.0
//
// MediaLibrary: the media list the browser shows. It keeps the items the player can open,
// groups them into sources (All media, Camera, Downloads, then one per album) and answers
// what a source holds under the view options. Pure logic, render thread.
#pragma once

#include "browser/MediaItem.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mp::browser {

enum class SourceKind { All, Camera, Downloads, Album };

struct Source {
    SourceKind kind = SourceKind::All;
    int64_t albumId = 0;  // Album only
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
    // Replace the list. Items the player cannot open (MediaSource::IsSupported) are dropped.
    void SetItems(std::vector<MediaItem> items);

    const std::vector<MediaItem>& Items() const { return items_; }
    // All media, Camera and Downloads, then the other albums by name. Camera and Downloads
    // items are not repeated in an album.
    const std::vector<Source>& Sources() const { return sources_; }
    // Indices into Items() of what `source` holds under `options`, in display order.
    std::vector<size_t> Query(const Source& source, const ViewOptions& options) const;

private:
    std::vector<MediaItem> items_;
    std::vector<Source> sources_;
};

} // namespace mp::browser
