// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the media browser's MediaLibrary (#160): what it counts as unsupported (the
// player's supported set, with HEIC switched off as on Android), the sources it
// builds (All media, Camera, Downloads, Pictures, Movies, Unsupported), what Query()
// returns for a source under the type filter and sort order, and the Downloads folders.
// Pure logic: no Vulkan, no JNI.

#include "browser/MediaLibrary.h"

#include <cstdio>
#include <string>
#include <vector>

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

MediaItem Item(int64_t id, const std::string& name, MediaType type, int64_t dateMs, const std::string& folder) {
    MediaItem item;
    item.id = id;
    item.name = name;
    item.type = type;
    item.dateMs = dateMs;
    item.folder = folder;
    return item;
}

std::vector<int64_t> Ids(const MediaLibrary& library, const std::vector<size_t>& indices) {
    std::vector<int64_t> ids;
    for (size_t i : indices) ids.push_back(library.Items()[i].id);
    return ids;
}

const Source* Find(const MediaLibrary& library, SourceKind kind) {
    for (const Source& s : library.Sources()) {
        if (s.kind == kind) return &s;
    }
    return nullptr;
}

void TestSupportedSet() {
    MediaLibrary library;
    library.SetItems({
        Item(1, "a.jpg", MediaType::Image, 1, ""),
        Item(2, "b.HEIC", MediaType::Image, 2, ""),
        Item(3, "c.mpo", MediaType::Image, 3, ""),
        Item(4, "d.mp4", MediaType::Video, 4, ""),
        Item(5, "e.webp", MediaType::Image, 5, ""),
        Item(6, "no_extension", MediaType::Image, 6, ""),
        Item(7, "f_2x1.PNG", MediaType::Image, 7, ""),
    });
    const std::vector<Source>& s = library.Sources();
    CHECK(library.Items().size() == 7, "keeps every item");
    CHECK(s[0].count == 4, "All media: jpg, mpo, mp4 and png");
    CHECK(s[5].count == 3, "Unsupported: heic, webp, no extension");
    CHECK(library.Kind(1) == SourceKind::Unsupported && library.Kind(0) == SourceKind::Pictures, "Kind");
    CHECK(library.Find(2) && library.Find(2)->name == "b.HEIC", "finds an unsupported item by id");
    CHECK(!library.Find(99), "no unknown item");
    CHECK((Ids(library, library.Query(s[0], ViewOptions{})) == std::vector<int64_t>{7, 4, 3, 1}),
          "All media leaves out what the player cannot open");
    CHECK((Ids(library, library.Query(s[5], ViewOptions{})) == std::vector<int64_t>{6, 5, 2}), "Unsupported");
}

void TestSources() {
    MediaLibrary library;
    library.SetItems({
        Item(1, "cam1.jpg", MediaType::Image, 10, "DCIM/Camera/"),
        Item(2, "cam2.mp4", MediaType::Video, 20, "DCIM/Camera/"),
        Item(3, "3d.jpg", MediaType::Image, 25, "DCIM/Camera/3D Photo/"),
        Item(4, "dl.jpg", MediaType::Image, 30, "Download/"),
        Item(5, "dl_sub.mp4", MediaType::Video, 40, "Download/trip/"),
        Item(6, "p.jpg", MediaType::Image, 50, "Pictures/"),
        Item(7, "shot.png", MediaType::Image, 60, "DCIM/Screenshots/"),
        Item(8, "m.mp4", MediaType::Video, 70, "Movies/clips/"),
        Item(9, "clip.mkv", MediaType::Video, 80, "TestMedia/"),
        Item(10, "root.jpg", MediaType::Image, 90, ""),
        Item(11, "art.webp", MediaType::Image, 100, "Pictures/"),
        Item(12, "cam.heic", MediaType::Image, 110, "DCIM/Camera/"),
    });
    const std::vector<Source>& s = library.Sources();
    const SourceKind kinds[] = {SourceKind::All,      SourceKind::Camera, SourceKind::Downloads,
                                SourceKind::Pictures, SourceKind::Movies, SourceKind::Unsupported};
    const char* names[] = {"All media", "Camera", "Downloads", "Pictures", "Movies", "Unsupported"};
    const int counts[] = {10, 3, 2, 3, 2, 2};
    CHECK(s.size() == 6, "six sources");
    for (size_t i = 0; i < 6 && i < s.size(); ++i) {
        CHECK(s[i].kind == kinds[i] && s[i].name == names[i], "sources in SourceKind order");
        CHECK(s[i].count == counts[i], names[i]);
    }

    library.SetItems({});
    CHECK(library.Sources().size() == 6 && library.Sources()[0].count == 0, "an empty library still has all six");
}

void TestQuery() {
    MediaLibrary library;
    library.SetItems({
        Item(1, "cam1.jpg", MediaType::Image, 10, "DCIM/Camera/"),
        Item(2, "cam2.mp4", MediaType::Video, 30, "DCIM/Camera/"),
        Item(3, "cam3.jpg", MediaType::Image, 20, "DCIM/Camera/"),
        Item(4, "same_date_a.jpg", MediaType::Image, 20, "Pictures/Art/"),
        Item(5, "art.mp4", MediaType::Video, 5, "Pictures/Art/"),
        Item(6, "doc.jpg", MediaType::Image, 1, "Documents/"),
    });
    const Source* all = Find(library, SourceKind::All);
    const Source* camera = Find(library, SourceKind::Camera);
    const Source* pictures = Find(library, SourceKind::Pictures);
    const Source* movies = Find(library, SourceKind::Movies);
    if (!all || !camera || !pictures || !movies) {
        CHECK(false, "sources present");
        return;
    }

    ViewOptions options;
    CHECK((Ids(library, library.Query(*all, options)) == std::vector<int64_t>{2, 4, 3, 1, 5, 6}),
          "newest first; equal dates by id, highest first");
    options.order = SortOrder::OldestFirst;
    CHECK((Ids(library, library.Query(*all, options)) == std::vector<int64_t>{6, 5, 1, 3, 4, 2}),
          "oldest first; equal dates by id, lowest first");

    options = ViewOptions{};
    options.type = TypeFilter::Videos;
    CHECK((Ids(library, library.Query(*all, options)) == std::vector<int64_t>{2, 5}), "videos only");
    options.type = TypeFilter::Images;
    CHECK((Ids(library, library.Query(*camera, options)) == std::vector<int64_t>{3, 1}), "Camera images only");
    options.type = TypeFilter::All;
    CHECK((Ids(library, library.Query(*pictures, options)) == std::vector<int64_t>{4, 6}), "Pictures: images anywhere");
    CHECK((Ids(library, library.Query(*movies, options)) == std::vector<int64_t>{5}), "Movies: videos anywhere");
}

void TestFolders() {
    MediaLibrary library;
    library.SetItems({
        Item(1, "top.jpg", MediaType::Image, 10, "Download/"),
        Item(2, "a.jpg", MediaType::Image, 20, "Download/MEDIA/ZTEPics/"),
        Item(3, "b.mp4", MediaType::Video, 30, "Download/MEDIA/ZTEPics/"),
        Item(4, "c.jpg", MediaType::Image, 40, "Download/MEDIA/"),
        Item(5, "d.jpg", MediaType::Image, 50, "Download/apps/"),
        Item(6, "e.webp", MediaType::Image, 60, "Download/webp/"),
        Item(7, "f.jpg", MediaType::Image, 70, "Download/zoo/"),
        Item(8, "pic.jpg", MediaType::Image, 80, "Pictures/"),
    });
    const Source* downloads = Find(library, SourceKind::Downloads);
    if (!downloads) {
        CHECK(false, "Downloads present");
        return;
    }
    CHECK(downloads->folder == "Download/", "Downloads is browsed as folders from Download/");
    CHECK(Find(library, SourceKind::All)->folder.empty(), "All media is one flat list");

    ViewOptions options;
    std::vector<mp::browser::Folder> f = library.Folders(*downloads, options, downloads->folder);
    CHECK(f.size() == 3, "MEDIA, apps and zoo; the webp folder holds nothing the player opens");
    if (f.size() == 3) {
        CHECK(f[0].name == "apps" && f[1].name == "MEDIA" && f[2].name == "zoo", "by name, case ignored");
        CHECK(f[1].path == "Download/MEDIA/", "the path ends in a slash");
        CHECK(f[1].count == 3, "a count takes in the sub-folders");
    }
    CHECK((Ids(library, library.Query(*downloads, options, "Download/")) == std::vector<int64_t>{1}),
          "a folder holds only what is directly in it");
    CHECK((Ids(library, library.Query(*downloads, options)) == std::vector<int64_t>{7, 5, 4, 3, 2, 1}),
          "no folder: the whole source");

    f = library.Folders(*downloads, options, "Download/MEDIA/");
    CHECK(f.size() == 1 && f[0].name == "ZTEPics" && f[0].path == "Download/MEDIA/ZTEPics/", "one level down");
    CHECK(library.Folders(*downloads, options, "Download/MEDIA/ZTEPics/").empty(), "a leaf has no sub-folders");

    options.type = TypeFilter::Videos;
    f = library.Folders(*downloads, options, "Download/");
    CHECK(f.size() == 1 && f[0].name == "MEDIA" && f[0].count == 1, "the type filter applies to folders");
}

}  // namespace

int main() {
    TestSupportedSet();
    TestSources();
    TestQuery();
    TestFolders();
    if (g_failures) {
        std::fprintf(stderr, "media_library_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("media_library_test: all passed\n");
    return 0;
}
