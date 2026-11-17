// SPDX-License-Identifier: Apache-2.0
#include "browser/MediaLibrary.h"

#include "media/MediaSource.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace mp::browser {

const char* const kNewestFolder = "\x01newest/";

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

// The last part of a folder path ("a/b/c/" -> "c"); a drive root keeps its colon ("C:").
std::string LastPart(const std::string& folder) {
    if (folder.empty()) return folder;
    const size_t end = folder.back() == '/' ? folder.size() - 1 : folder.size();
    const size_t cut = folder.rfind('/', end ? end - 1 : 0);
    return folder.substr(cut == std::string::npos ? 0 : cut + 1, end - (cut == std::string::npos ? 0 : cut + 1));
}

// The folder one up ("a/b/c/" -> "a/b/"), or "" at the top.
std::string UpOne(const std::string& folder) {
    if (folder.size() < 2) return std::string();
    const size_t cut = folder.rfind('/', folder.size() - 2);
    return cut == std::string::npos ? std::string() : folder.substr(0, cut + 1);
}

bool ListsFolders(const Source& s) {
    return s.mode == SourceMode::Grouped || (s.mode == SourceMode::Tree && s.roots.size() > 1);
}

} // namespace

void MediaLibrary::SetItems(std::vector<MediaItem> items) {
    items_ = std::move(items);
    kinds_.clear();
    byId_.clear();
    if (declared_.empty()) {
        sources_ = {{SourceKind::All, "All media", 0, ""},
                    {SourceKind::Camera, "Camera", 0, ""},
                    {SourceKind::Downloads, "Downloads", 0, kDownloads},
                    {SourceKind::Pictures, "Pictures", 0, ""},
                    {SourceKind::Movies, "Movies", 0, ""},
                    {SourceKind::Unsupported, "Unsupported", 0, ""}};
        sources_[0].dateSections = sources_[1].dateSections = true;  // the camera rolls (#180)
        for (size_t i = 0; i < items_.size(); ++i) {
            const SourceKind kind = KindOf(items_[i]);
            byId_[items_[i].id] = i;
            kinds_.push_back(kind);
            ++sources_[(size_t)kind].count;
            if (kind != SourceKind::Unsupported) ++sources_[(size_t)SourceKind::All].count;
        }
        return;
    }
    sources_ = declared_;
    for (Source& s : sources_) s.count = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        byId_[items_[i].id] = i;
        const bool supported = MediaSource::IsSupported(items_[i].name);
        kinds_.push_back(supported ? SourceKind::All : SourceKind::Unsupported);
        for (Source& s : sources_) {
            if (s.kind == SourceKind::Unsupported ? !supported
                                                  : supported && s.bit >= 0 && (items_[i].sources >> s.bit & 1u))
                ++s.count;
        }
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
    if (source.bit < 0 && source.kind != SourceKind::Unsupported && !declared_.empty()) return false;
    if (source.kind == SourceKind::Unsupported) return kinds_[i] == SourceKind::Unsupported;
    if (source.bit >= 0) return kinds_[i] != SourceKind::Unsupported && (item.sources >> source.bit & 1u);
    return source.kind == SourceKind::All ? kinds_[i] != SourceKind::Unsupported : kinds_[i] == source.kind;
}

void MediaLibrary::Sort(const Source& source, const ViewOptions& options, std::vector<size_t>& out) const {
    if (source.byOrder) {
        std::sort(out.begin(), out.end(), [&](size_t a, size_t b) { return items_[a].order < items_[b].order; });
        return;
    }
    const bool newest = options.order == SortOrder::NewestFirst;
    std::sort(out.begin(), out.end(), [&](size_t a, size_t b) {
        const MediaItem& x = items_[a];
        const MediaItem& y = items_[b];
        if (x.dateMs != y.dateMs) return newest ? x.dateMs > y.dateMs : x.dateMs < y.dateMs;
        return newest ? x.id > y.id : x.id < y.id;  // a stable order for equal dates
    });
}

std::vector<size_t> MediaLibrary::Query(const Source& source, const ViewOptions& options,
                                        const std::string& folder) const {
    std::vector<size_t> out;
    if (folder.empty() && ListsFolders(source)) return out;  // the top lists folders only
    if (source.mode == SourceMode::Grouped && folder == kNewestFolder) {
        for (size_t i = 0; i < items_.size(); ++i)
            if (Holds(source, options, i)) out.push_back(i);
        Sort(source, ViewOptions{options.type, SortOrder::NewestFirst}, out);
        if (out.size() > (size_t)std::max(0, source.newest)) out.resize((size_t)std::max(0, source.newest));
        return out;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!folder.empty() && items_[i].folder != folder) continue;
        if (Holds(source, options, i)) out.push_back(i);
    }
    Sort(source, options, out);
    return out;
}

std::vector<Folder> MediaLibrary::Folders(const Source& source, const ViewOptions& options,
                                          const std::string& folder) const {
    std::vector<Folder> out;
    if (source.mode == SourceMode::Grouped) {
        if (!folder.empty()) return out;
        std::map<std::string, int> counts;  // by path
        for (size_t i = 0; i < items_.size(); ++i)
            if (Holds(source, options, i)) ++counts[items_[i].folder];
        for (const auto& c : counts) {
            const std::string parent = UpOne(c.first);
            std::string detail = LastPart(parent);
            const std::string grand = LastPart(UpOne(parent));
            if (!grand.empty() && !detail.empty()) detail = grand + "/" + detail;
            out.push_back(Folder{LastPart(c.first), c.first, c.second, detail});
        }
        std::sort(out.begin(), out.end(), [](const Folder& a, const Folder& b) {
            return LessNoCase(a.name, b.name) || (!LessNoCase(b.name, a.name) && LessNoCase(a.path, b.path));
        });
        if (source.newest > 0 && !out.empty()) {
            int total = 0;
            for (const Folder& f : out) total += f.count;
            out.insert(out.begin(), Folder{"Newest", kNewestFolder, std::min(total, source.newest), ""});
        }
        return out;
    }
    if (folder.empty() && source.mode == SourceMode::Tree && source.roots.size() > 1) {
        for (const std::string& root : source.roots) {
            int count = 0;
            for (size_t i = 0; i < items_.size(); ++i)
                if (Under(items_[i], root) && Holds(source, options, i)) ++count;
            if (count) out.push_back(Folder{LastPart(root), root, count, LastPart(UpOne(root))});
        }
        return out;
    }
    std::map<std::string, int> counts;  // by name
    for (size_t i = 0; i < items_.size(); ++i) {
        const std::string& path = items_[i].folder;
        if (path.size() <= folder.size() || !Under(items_[i], folder) || !Holds(source, options, i)) continue;
        const size_t slash = path.find('/', folder.size());
        ++counts[path.substr(folder.size(), slash == std::string::npos ? std::string::npos : slash - folder.size())];
    }
    for (const auto& c : counts) out.push_back(Folder{c.first, folder + c.first + "/", c.second, ""});
    std::sort(out.begin(), out.end(), [](const Folder& a, const Folder& b) { return LessNoCase(a.name, b.name); });
    return out;
}

std::string MediaLibrary::Parent(const Source& source, const std::string& folder) const {
    if (folder == source.folder) return folder;
    if (source.mode == SourceMode::Grouped) return source.folder;
    if (source.roots.size() > 1 &&
        std::find(source.roots.begin(), source.roots.end(), folder) != source.roots.end())
        return source.folder;
    return UpOne(folder);
}

std::vector<std::string> MediaLibrary::Breadcrumb(const Source& source, const std::string& folder) const {
    std::vector<std::string> out;
    if (folder == source.folder) return out;
    if (source.mode == SourceMode::Grouped) {
        out.push_back(folder == kNewestFolder ? "Newest" : LastPart(folder));
        return out;
    }
    size_t i = source.folder.size();
    if (source.roots.size() > 1) {
        for (const std::string& root : source.roots) {
            if (folder.compare(0, root.size(), root) != 0) continue;
            out.push_back(LastPart(root));
            i = root.size();
            break;
        }
    }
    for (size_t slash; (slash = folder.find('/', i)) != std::string::npos; i = slash + 1)
        out.push_back(folder.substr(i, slash - i));
    return out;
}

} // namespace mp::browser
