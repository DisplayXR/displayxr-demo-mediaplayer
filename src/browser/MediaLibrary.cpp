// SPDX-License-Identifier: Apache-2.0
#include "browser/MediaLibrary.h"

#include "media/MediaSource.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace mp::browser {

namespace {

bool IsCamera(const MediaItem& item) { return item.folder == "DCIM/Camera/"; }
bool IsDownload(const MediaItem& item) { return item.folder.compare(0, 9, "Download/") == 0; }

bool InSource(const MediaItem& item, const Source& source) {
    switch (source.kind) {
        case SourceKind::All: return true;
        case SourceKind::Camera: return IsCamera(item);
        case SourceKind::Downloads: return IsDownload(item);
        case SourceKind::Album: return item.albumId == source.albumId && !IsCamera(item) && !IsDownload(item);
    }
    return false;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

void MediaLibrary::SetItems(std::vector<MediaItem> items) {
    items_.clear();
    for (MediaItem& item : items) {
        if (MediaSource::IsSupported(item.name)) items_.push_back(std::move(item));
    }

    Source camera{SourceKind::Camera, 0, "Camera", 0};
    Source downloads{SourceKind::Downloads, 0, "Downloads", 0};
    std::map<int64_t, Source> albums;
    for (const MediaItem& item : items_) {
        if (IsCamera(item)) {
            ++camera.count;
        } else if (IsDownload(item)) {
            ++downloads.count;
        } else if (!item.album.empty()) {
            Source& album = albums[item.albumId];
            album.kind = SourceKind::Album;
            album.albumId = item.albumId;
            album.name = item.album;
            ++album.count;
        }
    }

    sources_ = {Source{SourceKind::All, 0, "All media", (int)items_.size()}, camera, downloads};
    const size_t firstAlbum = sources_.size();
    for (const auto& entry : albums) sources_.push_back(entry.second);
    std::sort(sources_.begin() + firstAlbum, sources_.end(),
              [](const Source& a, const Source& b) { return Lower(a.name) < Lower(b.name); });
}

std::vector<size_t> MediaLibrary::Query(const Source& source, const ViewOptions& options) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < items_.size(); ++i) {
        const MediaItem& item = items_[i];
        if (options.type == TypeFilter::Videos && item.type != MediaType::Video) continue;
        if (options.type == TypeFilter::Images && item.type != MediaType::Image) continue;
        if (InSource(item, source)) out.push_back(i);
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
