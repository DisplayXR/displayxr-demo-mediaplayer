// SPDX-License-Identifier: Apache-2.0
#include "browser/MediaLibrary.h"

#include "media/MediaSource.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace mp::browser {

namespace {

const char* const kDownloads = "Download/";

bool Under(const MediaItem& item, const std::string& folder) {
    return item.folder.compare(0, folder.size(), folder) == 0;
}

SourceKind KindOf(const MediaItem& item) {
    if (!MediaSource::IsSupported(item.name)) return SourceKind::Unsupported;
    if (Under(item, "DCIM/Camera/")) return SourceKind::Camera;
    if (Under(item, kDownloads)) return SourceKind::Downloads;
    return item.type == MediaType::Video ? SourceKind::Movies : SourceKind::Pictures;
}

bool LessNoCase(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
    });
}

} // namespace

void MediaLibrary::SetItems(std::vector<MediaItem> items) {
    items_ = std::move(items);
    kinds_.clear();
    byId_.clear();
    sources_ = {{SourceKind::All, "All media", 0, ""},
                {SourceKind::Camera, "Camera", 0, ""},
                {SourceKind::Downloads, "Downloads", 0, kDownloads},
                {SourceKind::Pictures, "Pictures", 0, ""},
                {SourceKind::Movies, "Movies", 0, ""},
                {SourceKind::Unsupported, "Unsupported", 0, ""}};
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

bool MediaLibrary::Holds(const Source& source, const ViewOptions& options, size_t i) const {
    const MediaItem& item = items_[i];
    if (options.type == TypeFilter::Videos && item.type != MediaType::Video) return false;
    if (options.type == TypeFilter::Images && item.type != MediaType::Image) return false;
    return source.kind == SourceKind::All ? kinds_[i] != SourceKind::Unsupported : kinds_[i] == source.kind;
}

std::vector<size_t> MediaLibrary::Query(const Source& source, const ViewOptions& options,
                                        const std::string& folder) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!folder.empty() && items_[i].folder != folder) continue;
        if (Holds(source, options, i)) out.push_back(i);
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

std::vector<Folder> MediaLibrary::Folders(const Source& source, const ViewOptions& options,
                                          const std::string& folder) const {
    std::map<std::string, int> counts;  // by name
    for (size_t i = 0; i < items_.size(); ++i) {
        const std::string& path = items_[i].folder;
        if (path.size() <= folder.size() || !Under(items_[i], folder) || !Holds(source, options, i)) continue;
        const size_t slash = path.find('/', folder.size());
        ++counts[path.substr(folder.size(), slash == std::string::npos ? std::string::npos : slash - folder.size())];
    }
    std::vector<Folder> out;
    for (const auto& c : counts) out.push_back(Folder{c.first, folder + c.first + "/", c.second});
    std::sort(out.begin(), out.end(), [](const Folder& a, const Folder& b) { return LessNoCase(a.name, b.name); });
    return out;
}

} // namespace mp::browser
