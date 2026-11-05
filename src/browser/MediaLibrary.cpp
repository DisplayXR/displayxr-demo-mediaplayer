// SPDX-License-Identifier: Apache-2.0
#include "browser/MediaLibrary.h"

#include "media/MediaSource.h"

#include <algorithm>

namespace mp::browser {

namespace {

bool Under(const MediaItem& item, const std::string& folder) {
    return item.folder.compare(0, folder.size(), folder) == 0;
}

SourceKind KindOf(const MediaItem& item) {
    if (!MediaSource::IsSupported(item.name)) return SourceKind::Unsupported;
    if (Under(item, "DCIM/Camera/")) return SourceKind::Camera;
    if (Under(item, "Download/")) return SourceKind::Downloads;
    return item.type == MediaType::Video ? SourceKind::Movies : SourceKind::Pictures;
}

} // namespace

void MediaLibrary::SetItems(std::vector<MediaItem> items) {
    items_ = std::move(items);
    kinds_.clear();
    byId_.clear();
    sources_ = {{SourceKind::All, "All media", 0},
                {SourceKind::Camera, "Camera", 0},
                {SourceKind::Downloads, "Downloads", 0},
                {SourceKind::Pictures, "Pictures", 0},
                {SourceKind::Movies, "Movies", 0},
                {SourceKind::Unsupported, "Unsupported", 0}};
    for (size_t i = 0; i < items_.size(); ++i) {
        const SourceKind kind = KindOf(items_[i]);
        byId_[items_[i].id] = i;
        kinds_.push_back(kind);
        ++sources_[(size_t)kind].count;
        if (kind != SourceKind::Unsupported) ++sources_[(size_t)SourceKind::All].count;
    }
}

const MediaItem* MediaLibrary::Find(int64_t id) const {
    const auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &items_[it->second];
}

std::vector<size_t> MediaLibrary::Query(const Source& source, const ViewOptions& options) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < items_.size(); ++i) {
        const MediaItem& item = items_[i];
        if (options.type == TypeFilter::Videos && item.type != MediaType::Video) continue;
        if (options.type == TypeFilter::Images && item.type != MediaType::Image) continue;
        const bool in = source.kind == SourceKind::All ? kinds_[i] != SourceKind::Unsupported : kinds_[i] == source.kind;
        if (in) out.push_back(i);
    }
    const bool newest = options.order == SortOrder::NewestFirst;
    std::sort(out.begin(), out.end(), [&](size_t a, size_t b) {
        const MediaItem& x = items_[a];
        const MediaItem& y = items_[b];
        if (x.dateMs != y.dateMs) return newest ? x.dateMs > y.dateMs : x.dateMs < y.dateMs;
        return newest ? x.id > y.id : x.id < y.id;  // a stable order for equal dates
    });
    return out;
}

} // namespace mp::browser
