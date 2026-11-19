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

// Declared sources (desktop, #252): membership by bit, Recents in provider order, a Tree
// with several roots, a Grouped source with its Newest tile, and the unsupported routing.
MediaItem Desk(int64_t id, const std::string& name, MediaType type, int64_t dateMs, const std::string& folder,
               uint64_t bits, int order = 0) {
    MediaItem item = Item(id, name, type, dateMs, folder);
    item.sources = bits;
    item.order = order;
    return item;
}

void TestDeclaredSources() {
    using mp::browser::Folder;
    using mp::browser::kNewestFolder;
    using mp::browser::SourceMode;
    std::vector<Source> declared(5);
    declared[0].kind = SourceKind::Recents, declared[0].name = "Recents", declared[0].bit = 0, declared[0].byOrder = true;
    declared[1].kind = SourceKind::Pictures, declared[1].name = "Pictures", declared[1].bit = 1;
    declared[1].mode = SourceMode::Tree, declared[1].roots = {"C:/P/", "D:/OneDrive/Pictures/"};
    declared[2].kind = SourceKind::Folders, declared[2].name = "Media folders", declared[2].bit = 2;
    declared[2].mode = SourceMode::Grouped, declared[2].newest = 2;
    declared[3].kind = SourceKind::Downloads, declared[3].name = "Downloads", declared[3].bit = 3;
    declared[3].mode = SourceMode::Tree, declared[3].folder = "C:/Dl/";
    declared[4].kind = SourceKind::Unsupported, declared[4].name = "Unsupported";
    MediaLibrary library;
    library.SetSources(declared);
    library.SetItems({
        Desk(1, "a.jpg", MediaType::Image, 10, "C:/P/", 0b0011, 1),
        Desk(2, "b.jpg", MediaType::Image, 20, "C:/P/trip/", 0b0010),
        Desk(3, "c.mp4", MediaType::Video, 30, "D:/OneDrive/Pictures/", 0b0111, 0),
        Desk(4, "d.jpg", MediaType::Image, 40, "E:/Team/MEDIA/", 0b0100),
        Desk(5, "e.jpg", MediaType::Image, 50, "F:/Other/MEDIA/", 0b0100),
        Desk(6, "f.xyz", MediaType::Image, 60, "C:/Dl/", 0b1000),
        Desk(7, "g.jpg", MediaType::Image, 70, "C:/Dl/sub/", 0b1000),
    });
    const std::vector<Source>& s = library.Sources();
    CHECK(s.size() == 5 && s[0].count == 2 && s[1].count == 3 && s[2].count == 3 && s[3].count == 1 &&
              s[4].count == 1,
          "declared sources count by bit; the unsupported file only in Unsupported");
    const ViewOptions all;
    CHECK((Ids(library, library.Query(s[0], all)) == std::vector<int64_t>{3, 1}), "Recents in provider order");

    // Pictures: two roots, listed first; inside a root, a normal tree.
    CHECK(library.Query(s[1], all).empty(), "a multi-root top shows only its roots");
    std::vector<Folder> roots = library.Folders(s[1], all, "");
    CHECK(roots.size() == 2 && roots[0].path == "C:/P/" && roots[0].count == 2 && roots[1].name == "Pictures",
          "the roots, with their counts");
    CHECK((Ids(library, library.Query(s[1], all, "C:/P/")) == std::vector<int64_t>{1}), "a root's own files");
    std::vector<Folder> sub = library.Folders(s[1], all, "C:/P/");
    CHECK(sub.size() == 1 && sub[0].name == "trip", "a root's sub-folders");
    CHECK(library.Parent(s[1], "C:/P/trip/") == "C:/P/" && library.Parent(s[1], "C:/P/").empty(),
          "up from a root goes to the roots");
    CHECK((library.Breadcrumb(s[1], "C:/P/trip/") == std::vector<std::string>{"P", "trip"}), "breadcrumb from a root");

    // Media folders: one tile per folder, Newest first; folders that share a name tell apart.
    std::vector<Folder> groups = library.Folders(s[2], all, "");
    CHECK(groups.size() == 4 && groups[0].path == kNewestFolder && groups[0].count == 2, "the Newest tile first");
    CHECK(groups[1].name == "MEDIA" && groups[2].name == "MEDIA" && groups[1].detail != groups[2].detail,
          "same-named folders differ in detail");
    CHECK((Ids(library, library.Query(s[2], all, kNewestFolder)) == std::vector<int64_t>{5, 4}),
          "Newest: the newest few, newest first");
    CHECK((Ids(library, library.Query(s[2], all, "E:/Team/MEDIA/")) == std::vector<int64_t>{4}), "a group's files");
    CHECK(library.Folders(s[2], all, "E:/Team/MEDIA/").empty(), "a group has no sub-folders");
    CHECK(library.Parent(s[2], "E:/Team/MEDIA/").empty(), "up from a group goes to the list");

    // Downloads: one root, browsed from inside it (as on Android).
    CHECK((Ids(library, library.Query(s[3], all, "C:/Dl/")).empty()), "the unsupported file is not in Downloads");
    CHECK(library.Folders(s[3], all, "C:/Dl/").size() == 1, "Downloads' sub-folder");
    CHECK(!library.Supported(5) && library.Supported(0), "Supported");
}

// Folder previews (#252 desktop): what a folder card shows a mosaic of.
void TestFolderPreviews() {
    using mp::browser::Folder;
    using mp::browser::kNewestFolder;
    using mp::browser::SourceMode;
    std::vector<Source> declared(2);
    declared[0].kind = SourceKind::Pictures, declared[0].name = "Pictures", declared[0].bit = 0;
    declared[0].mode = SourceMode::Tree, declared[0].folder = "C:/P/";
    declared[1].kind = SourceKind::Folders, declared[1].name = "Media folders", declared[1].bit = 1;
    declared[1].mode = SourceMode::Grouped, declared[1].newest = 3;
    MediaLibrary library;
    library.SetSources(declared);
    MediaItem cloud = Desk(4, "d.jpg", MediaType::Image, 90, "C:/P/trip/", 0b01);
    cloud.cloudOnly = true;
    library.SetItems({
        Desk(1, "a.jpg", MediaType::Image, 10, "C:/P/trip/", 0b11),
        Desk(2, "b.jpg", MediaType::Image, 30, "C:/P/trip/day1/", 0b11),
        Desk(3, "c.mp4", MediaType::Video, 20, "C:/P/trip/", 0b11),
        cloud,
        Desk(5, "e.jpg", MediaType::Image, 50, "C:/P/trip/", 0b01),
        Desk(6, "f.jpg", MediaType::Image, 60, "C:/P/work/", 0b01),
        Desk(7, "g.xyz", MediaType::Image, 70, "C:/P/work/", 0b01),
    });
    const std::vector<Source>& s = library.Sources();
    const ViewOptions all;
    const std::vector<Folder> tree = library.Folders(s[0], all, "C:/P/");
    std::vector<std::vector<size_t>> p = library.FolderPreviews(s[0], all, tree, 4);
    CHECK(tree.size() == 2 && p.size() == 2, "one preview per folder");
    CHECK((Ids(library, p[0]) == std::vector<int64_t>{5, 2, 3, 1}), "a tree: its sub-folders too, local first, newest first");
    CHECK((Ids(library, p[1]) == std::vector<int64_t>{6}), "an unsupported file is never a preview");
    p = library.FolderPreviews(s[0], all, tree, 5);
    CHECK((Ids(library, p[0]) == std::vector<int64_t>{5, 2, 3, 1, 4}), "a cloud-only file after the local ones");
    ViewOptions videos;
    videos.type = TypeFilter::Videos;
    CHECK((Ids(library, library.FolderPreviews(s[0], videos, tree, 4)[0]) == std::vector<int64_t>{3}),
          "under the type filter");
    const std::vector<Folder> groups = library.Folders(s[1], all, "");
    p = library.FolderPreviews(s[1], all, groups, 4);
    CHECK(groups.size() == 3 && groups[0].path == kNewestFolder, "Newest, day1, trip");
    CHECK((Ids(library, p[0]) == std::vector<int64_t>{2, 3, 1}), "the Newest tile: its own");
    CHECK((Ids(library, p[1]) == std::vector<int64_t>{2}) && (Ids(library, p[2]) == std::vector<int64_t>{3, 1}),
          "a group: only the files directly in it");
    CHECK(library.FolderPreviews(s[0], all, tree, 0)[0].empty(), "max 0: none");
}

int main() {
    TestSupportedSet();
    TestSources();
    TestQuery();
    TestFolders();
    TestDeclaredSources();
    TestFolderPreviews();
    if (g_failures) {
        std::fprintf(stderr, "media_library_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("media_library_test: all passed\n");
    return 0;
}
