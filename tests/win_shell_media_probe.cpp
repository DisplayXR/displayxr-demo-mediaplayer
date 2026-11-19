// SPDX-License-Identifier: Apache-2.0
//
// win_shell_media_probe: a manual probe of WinShellMedia on a real Windows box (#252).
// Not a ctest test: what it finds depends on the machine. It prints the libraries, the
// index query's size and timing, and takes a few thumbnails, checking that a cloud-only
// file is still cloud-only afterwards (a thumbnail must never download the file).
//
//   win_shell_media_probe [out-dir]   (PNGs of the thumbnails go to out-dir)
#include "browser/WinShellMedia.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <map>
#include <string>

using namespace mp::browser::win;

static std::wstring W(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static bool AcceptMedia(const std::string& path, bool& video) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)tolower((unsigned char)c);
    video = e == "mp4" || e == "mov" || e == "mkv" || e == "webm" || e == "avi";
    return video || e == "jpg" || e == "jpeg" || e == "png" || e == "mpo" || e == "heic" || e == "webp";
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    ScopedCom com;
    // --thumb <path> <cacheOnly 0|1> [out.png]: one thumbnail, and whether the file is still
    // a cloud placeholder afterwards.
    if (argc >= 4 && std::string(argv[1]) == "--thumb") {
        const std::string path = argv[2];
        const bool cacheOnly = std::string(argv[3]) == "1";
        const DWORD before = GetFileAttributesW(W(path).c_str());
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
        const double t = NowMs();
        const bool got = ShellThumbnail(path, 512, cacheOnly, w, h, rgba);
        const DWORD after = GetFileAttributesW(W(path).c_str());
        std::printf("thumb %s %ux%u in %.0f ms; cloud before=%d after=%d\n", got ? "ok" : "none", w, h, NowMs() - t,
                    IsCloudOnly(before), IsCloudOnly(after));
        if (got && argc >= 5) stbi_write_png(argv[4], (int)w, (int)h, 4, rgba.data(), (int)w * 4);
        return 0;
    }
    const std::string outDir = argc > 1 ? argv[1] : "";
    std::printf("COM: %s\n", com.Ok() ? "ok" : "FAILED");

    for (Library lib : {Library::Pictures, Library::Videos}) {
        for (const std::string& f : LibraryFolders(lib)) {
            std::vector<FoundFile> files;
            const double t0 = NowMs();
            const bool complete = WalkFolder(f, 3, t0 + 2000.0, AcceptMedia, files);
            std::printf("%s library: %s  -> %zu files in %.0f ms%s\n", lib == Library::Pictures ? "Pictures" : "Videos",
                        f.c_str(), files.size(), NowMs() - t0, complete ? "" : " (cut short)");
        }
    }
    std::printf("Downloads: %s\n", DownloadsFolder().c_str());

    std::vector<FoundFile> found;
    std::string error;
    double t0 = NowMs();
    IndexSkipped skipped;
    const bool ok = QueryIndex(IndexQuery{}, found, error, &skipped);
    const double ms = NowMs() - t0;
    if (!ok) {
        std::printf("index: FAILED: %s\n", error.c_str());
        return 1;
    }
    std::map<std::string, int> folders;
    int cloud = 0, videos = 0;
    for (const FoundFile& f : found) {
        ++folders[f.folder];
        cloud += f.cloudOnly;
        videos += f.video;
    }
    std::printf("index: %zu items (%d videos, %d cloud-only) in %zu folders, %.0f ms\n", found.size(), videos, cloud,
                folders.size(), ms);
    std::printf("index left out: %d in code checkouts, %d in game libraries, %d in asset folders, %d pictures beside a "
                "program\n", skipped.code, skipped.library, skipped.assetFolder, skipped.appFolder);
    int shown = 0;
    for (auto it = folders.begin(); it != folders.end() && shown < 12; ++it, ++shown)
        std::printf("  %4d  %s\n", it->second, it->first.c_str());

    // Thumbnails: two local, three cloud-only (cache only, then allowed to make one).
    int local = 0, remote = 0, n = 0;
    for (const FoundFile& f : found) {
        if (f.cloudOnly ? remote >= 3 : local >= 2) continue;
        (f.cloudOnly ? remote : local)++;
        for (int pass = 0; pass < (f.cloudOnly ? 2 : 1); ++pass) {
            const bool cacheOnly = f.cloudOnly && pass == 0;
            uint32_t w = 0, h = 0;
            std::vector<uint8_t> rgba;
            t0 = NowMs();
            const bool got = ShellThumbnail(f.path, 256, cacheOnly, w, h, rgba);
            const double tms = NowMs() - t0;
            const DWORD after = GetFileAttributesW(W(f.path).c_str());
            std::printf("thumb %-5s %-10s %4ux%-4u %5.0f ms  still-cloud=%s  %s\n", got ? "ok" : "none",
                        f.cloudOnly ? (cacheOnly ? "cloud/cache" : "cloud/make") : "local", w, h, tms,
                        f.cloudOnly ? (IsCloudOnly(after) ? "yes" : "NO-HYDRATED") : "-", f.name.c_str());
            if (got && !outDir.empty()) {
                const std::string png = outDir + "\\thumb_" + std::to_string(n++) + ".png";
                stbi_write_png(png.c_str(), (int)w, (int)h, 4, rgba.data(), (int)w * 4);
            }
        }
        if (local >= 2 && remote >= 3) break;
    }
    return 0;
}
