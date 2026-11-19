// SPDX-License-Identifier: Apache-2.0
//
// WinShellMedia: what Windows itself knows about the user's media, without reading any
// file's bytes (#252). The Windows Search index stands in for Android's MediaStore, the
// shell libraries and known folders give the sidebar's fixed sources, and the shell's
// thumbnail cache gives pictures for cloud-only files without downloading them.
//
// Plain blocking calls: the caller runs them on its own thread, with COM initialized
// (ScopedCom). Windows only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mp::browser::win {

// COM for the calling thread (apartment-threaded, as the shell wants), released on exit.
class ScopedCom {
public:
    ScopedCom();
    ~ScopedCom();
    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;
    bool Ok() const { return ok_; }

private:
    bool ok_ = false;
    bool uninit_ = false;
};

// One file, as the index or a folder walk reports it. Paths are UTF-8.
struct FoundFile {
    std::string path;    // full path
    std::string folder;  // its folder, with a trailing separator
    std::string name;
    bool video = false;
    uint32_t width = 0, height = 0;  // px; 0 x 0 if unknown (a folder walk never knows)
    int64_t bytes = 0;
    int64_t dateMs = 0;      // date taken when the index has it, else last modified
    int64_t modified = 0;    // last modified, seconds since the epoch
    int64_t durationMs = 0;  // videos, when the index has it
    bool cloudOnly = false;  // a placeholder: reading its bytes would download it
};

// Placeholder attributes: FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS / _OFFLINE / _RECALL_ON_OPEN.
bool IsCloudOnly(uint32_t attributes);

// The folders of the Pictures or Videos library (OneDrive Pictures and user-added
// folders included). Falls back to the known folder alone.
enum class Library { Pictures, Videos };
std::vector<std::string> LibraryFolders(Library library);
// The Downloads known folder, or empty.
std::string DownloadsFolder();

// The Search index's pictures and videos, junk-filtered (#252 decisions): pictures with a
// long side >= minPictureEdge, videos of at least minVideoBytes; nothing under AppData,
// Program Files, Windows or a hidden folder. Nor what software keeps beside itself
// (#252): anything inside a git checkout or a game library (Epic, Steam, ...), under an
// asset-named folder (textures, icons, Models, *_Data, ...), and pictures in a folder
// that holds a program (.exe / .dll). Media folders lists the user's own media; Open
// file / Open folder, drop and pinned folders still reach everything. False (with
// `error`) when the index cannot be queried, e.g. the WSearch service is stopped.
struct IndexQuery {
    uint32_t minPictureEdge = 1024;
    int64_t minVideoBytes = 1 << 20;
    // Only under this folder (e.g. a library folder); empty = every indexed location.
    std::string scope;
};
// How many files each software rule left out (for the log).
struct IndexSkipped {
    int code = 0;         // inside a git checkout
    int library = 0;      // under a game library
    int assetFolder = 0;  // under an asset-named folder
    int appFolder = 0;    // a picture beside a program
};
bool QueryIndex(const IndexQuery& query, std::vector<FoundFile>& out, std::string& error,
                IndexSkipped* skipped = nullptr);

// Walk `folder` for files `accept` takes, at most `maxDepth` folders down (0 = only its
// own files), until `deadlineMs` (steady clock ms; 0 = none). Reads directory entries
// only. Returns false if the deadline cut it short.
bool WalkFolder(const std::string& folder, int maxDepth, double deadlineMs,
                bool (*accept)(const std::string& path, bool& video), std::vector<FoundFile>& out);

// A thumbnail of `path` fitting maxEdge x maxEdge, RGBA8, from the shell. With
// `cacheOnly` it comes only from the thumbnail cache (a cloud file is never downloaded);
// false then when the cache has none.
bool ShellThumbnail(const std::string& path, uint32_t maxEdge, bool cacheOnly, uint32_t& width,
                    uint32_t& height, std::vector<uint8_t>& rgba);

// Steady clock, ms.
double NowMs();

} // namespace mp::browser::win
