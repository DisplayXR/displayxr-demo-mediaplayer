// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the media browser's MediaLibrary (#160): which items it keeps (the
// player's supported set, with MPO and HEIC switched off as on Android), the sources it
// builds (All media, Camera, Downloads, albums by name) and what Query() returns for a
// source under the type filter and sort order. Pure logic: no Vulkan, no JNI.

#include "browser/MediaLibrary.h"
#include "media/MediaSource.h"

#include <cstdio>
#include <string>
#include <vector>

using mp::MediaSource;
using mp::browser::MediaItem;
using mp::browser::MediaLibrary;
using mp::browser::MediaType;
using mp::browser::SortOrder;
using mp::browser::Source;
using mp::browser::SourceKind;
using mp::browser::TypeFilter;
using mp::browser::ViewOptions;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

MediaItem Item(int64_t id, const std::string& name, MediaType type, int64_t dateMs, int64_t albumId,
               const std::string& album, const std::string& folder) {
    MediaItem item;
    item.id = id;
    item.name = name;
    item.type = type;
    item.dateMs = dateMs;
    item.albumId = albumId;
    item.album = album;
    item.folder = folder;
    return item;
}

std::vector<int64_t> Ids(const MediaLibrary& library, const std::vector<size_t>& indices) {
    std::vector<int64_t> ids;
    for (size_t i : indices) ids.push_back(library.Items()[i].id);
    return ids;
}

const Source* Find(const MediaLibrary& library, SourceKind kind, const std::string& name = "") {
    for (const Source& s : library.Sources()) {
        if (s.kind == kind && (name.empty() || s.name == name)) return &s;
    }
    return nullptr;
}

void TestSupportedSet() {
    MediaSource::SetMpoSupported(false);
    MediaLibrary library;
    library.SetItems({
        Item(1, "a.jpg", MediaType::Image, 1, 0, "", ""),
        Item(2, "b.HEIC", MediaType::Image, 2, 0, "", ""),
        Item(3, "c.mpo", MediaType::Image, 3, 0, "", ""),
        Item(4, "d.mp4", MediaType::Video, 4, 0, "", ""),
        Item(5, "e.webp", MediaType::Image, 5, 0, "", ""),
        Item(6, "no_extension", MediaType::Image, 6, 0, "", ""),
        Item(7, "f_2x1.PNG", MediaType::Image, 7, 0, "", ""),
    });
    CHECK(library.Items().size() == 3, "keeps jpg, mp4 and png; drops heic, mpo, webp, no extension");

    MediaSource::SetMpoSupported(true);
    library.SetItems({Item(3, "c.mpo", MediaType::Image, 3, 0, "", "")});
    CHECK(library.Items().size() == 1, "keeps mpo when MPO is supported");
    MediaSource::SetMpoSupported(false);
}

void TestSources() {
    MediaLibrary library;
    library.SetItems({
        Item(1, "cam1.jpg", MediaType::Image, 10, 100, "Camera", "DCIM/Camera/"),
        Item(2, "cam2.mp4", MediaType::Video, 20, 100, "Camera", "DCIM/Camera/"),
        Item(3, "dl.jpg", MediaType::Image, 30, 200, "Download", "Download/"),
        Item(4, "dl_sub.jpg", MediaType::Image, 40, 201, "trip", "Download/trip/"),
        Item(5, "z.jpg", MediaType::Image, 50, 300, "zoo", "Pictures/zoo/"),
        Item(6, "a.jpg", MediaType::Image, 60, 400, "Art", "Pictures/Art/"),
        Item(7, "a2.mp4", MediaType::Video, 70, 400, "Art", "Pictures/Art/"),
        Item(8, "root.jpg", MediaType::Image, 80, 500, "", ""),
    });
    const std::vector<Source>& s = library.Sources();
    CHECK(s.size() == 5, "All, Camera, Downloads and two albums");
    CHECK(s.size() == 5 && s[0].kind == SourceKind::All && s[0].count == 8, "All media first, counts everything");
    CHECK(s.size() == 5 && s[1].kind == SourceKind::Camera && s[1].count == 2, "Camera second");
    CHECK(s.size() == 5 && s[2].kind == SourceKind::Downloads && s[2].count == 2,
          "Downloads third, subfolders included");
    CHECK(s.size() == 5 && s[3].name == "Art" && s[3].count == 2, "albums by name, case-insensitive (Art)");
    CHECK(s.size() == 5 && s[4].name == "zoo" && s[4].count == 1, "albums by name, case-insensitive (zoo)");
    CHECK(Find(library, SourceKind::Album, "Camera") == nullptr, "Camera is not repeated as an album");
    CHECK(Find(library, SourceKind::Album, "trip") == nullptr, "a Downloads subfolder is not an album");

    library.SetItems({});
    CHECK(library.Sources().size() == 3, "an empty library still has All, Camera and Downloads");
}

void TestQuery() {
    MediaLibrary library;
    library.SetItems({
        Item(1, "cam1.jpg", MediaType::Image, 10, 100, "Camera", "DCIM/Camera/"),
        Item(2, "cam2.mp4", MediaType::Video, 30, 100, "Camera", "DCIM/Camera/"),
        Item(3, "cam3.jpg", MediaType::Image, 20, 100, "Camera", "DCIM/Camera/"),
        Item(4, "same_date_a.jpg", MediaType::Image, 20, 400, "Art", "Pictures/Art/"),
        Item(5, "art.mp4", MediaType::Video, 5, 400, "Art", "Pictures/Art/"),
    });
    const Source* all = Find(library, SourceKind::All);
    const Source* camera = Find(library, SourceKind::Camera);
    const Source* art = Find(library, SourceKind::Album, "Art");
    if (!all || !camera || !art) {
        CHECK(false, "sources present");
        return;
    }

    ViewOptions options;
    CHECK((Ids(library, library.Query(*all, options)) == std::vector<int64_t>{2, 4, 3, 1, 5}),
          "newest first; equal dates by id, highest first");
    options.order = SortOrder::OldestFirst;
    CHECK((Ids(library, library.Query(*all, options)) == std::vector<int64_t>{5, 1, 3, 4, 2}),
          "oldest first; equal dates by id, lowest first");

    options = ViewOptions{};
    options.type = TypeFilter::Videos;
    CHECK((Ids(library, library.Query(*all, options)) == std::vector<int64_t>{2, 5}), "videos only");
    options.type = TypeFilter::Images;
    CHECK((Ids(library, library.Query(*camera, options)) == std::vector<int64_t>{3, 1}), "Camera images only");
    options.type = TypeFilter::All;
    CHECK((Ids(library, library.Query(*art, options)) == std::vector<int64_t>{4, 5}), "one album");
}

}  // namespace

int main() {
    TestSupportedSet();
    TestSources();
    TestQuery();
    if (g_failures) {
        std::fprintf(stderr, "media_library_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("media_library_test: all passed\n");
    return 0;
}
