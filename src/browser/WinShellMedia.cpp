// SPDX-License-Identifier: Apache-2.0
#include "browser/WinShellMedia.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The OLE DB GUIDs (DBGUID_DEFAULT, ...) are defined in this one translation unit.
#define DBINITCONSTANTS
#include <oledb.h>
#include <oledberr.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace mp::browser::win {

namespace {

// Search.CollatorDSO, the Windows Search OLE DB provider. Spelled out so no SDK
// import library is needed for it.
const CLSID kCollatorDataSource = {0x9E175B8B, 0xF52A, 0x11D8, {0xB9, 0xA5, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}};

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Utf8(const wchar_t* w, size_t len) {
    if (!len) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, s.data(), n, nullptr, nullptr);
    return s;
}
std::string Utf8(const std::wstring& w) { return Utf8(w.data(), w.size()); }

int64_t FileTimeMs(const FILETIME& ft) {
    const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (!t) return 0;
    return (int64_t)(t / 10000ULL) - 11644473600000LL;  // 1601 -> 1970
}

// Split a full path into its folder (with the separator) and name.
void SplitPath(const std::string& path, std::string& folder, std::string& name) {
    const size_t cut = path.find_last_of("\\/");
    if (cut == std::string::npos) {
        folder.clear();
        name = path;
    } else {
        folder = path.substr(0, cut + 1);
        name = path.substr(cut + 1);
    }
}

// --- variants out of the index ---

int64_t VarInt(const VARIANT& v) {
    if (v.vt == VT_EMPTY || v.vt == VT_NULL) return 0;
    VARIANT t;
    VariantInit(&t);
    int64_t r = 0;
    if (SUCCEEDED(VariantChangeType(&t, &v, 0, VT_I8))) r = t.llVal;
    VariantClear(&t);
    return r;
}

int64_t VarDateMs(const VARIANT& v) {
    if (v.vt != VT_DATE) return 0;
    SYSTEMTIME st;
    FILETIME ft;
    if (!VariantTimeToSystemTime(v.date, &st) || !SystemTimeToFileTime(&st, &ft)) return 0;
    return FileTimeMs(ft);
}

std::string VarString(const VARIANT& v) {
    if (v.vt == VT_BSTR && v.bstrVal) return Utf8(v.bstrVal, SysStringLen(v.bstrVal));
    return {};
}

// Locations that hold application images, not the user's media.
bool ExcludedPath(const std::string& path) {
    static const char* const kSkip[] = {"\\appdata\\", "\\program files", "\\programdata\\",
                                        "\\windows\\", "\\$recycle.bin\\", "\\node_modules\\"};
    std::string lower(path);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (const char* s : kSkip)
        if (lower.find(s) != std::string::npos) return true;
    // A dot-folder anywhere on the way (".git", ".cache", ...).
    for (size_t i = lower.find("\\."); i != std::string::npos; i = lower.find("\\.", i + 1))
        if (lower.find('\\', i + 1) != std::string::npos) return true;
    return false;
}

// Whether `folder` or one of its parents is hidden. Cached: the index lists many files
// per folder. Reads folder attributes only (never hydrates anything).
class HiddenFolders {
public:
    bool Hidden(const std::string& folder) {
        auto it = cache_.find(folder);
        if (it != cache_.end()) return it->second;
        bool hidden = false;
        const std::wstring w = Wide(folder);
        const DWORD a = GetFileAttributesW(w.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) &&
            folder.size() > 3)  // a drive root reports SYSTEM|HIDDEN on some volumes
            hidden = true;
        if (!hidden) {
            // The parent (folder ends with a separator: cut that, then the last part).
            std::string trimmed = folder.substr(0, folder.size() - 1);
            const size_t cut = trimmed.find_last_of("\\/");
            if (cut != std::string::npos && cut + 1 > 3) hidden = Hidden(trimmed.substr(0, cut + 1));
        }
        cache_[folder] = hidden;
        return hidden;
    }

private:
    std::unordered_map<std::string, bool> cache_;
};

struct Cell {
    VARIANT value;
    DBSTATUS status;
    DBLENGTH length;
};

// Run one SQL query against the index; `row` gets each row's cells (in SELECT order).
bool RunQuery(const std::wstring& sql, size_t columns,
              const std::function<void(const std::vector<Cell>&)>& row, std::string& error) {
    ComPtr<IDBInitialize> init;
    HRESULT hr = CoCreateInstance(kCollatorDataSource, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&init));
    if (FAILED(hr)) {
        error = "the Windows Search provider is not available";
        return false;
    }
    if (FAILED(hr = init->Initialize())) {
        error = "the Windows Search provider did not start";
        return false;
    }
    ComPtr<IDBCreateSession> sessionFactory;
    ComPtr<IDBCreateCommand> commandFactory;
    ComPtr<ICommandText> command;
    if (FAILED(init.As(&sessionFactory)) ||
        FAILED(sessionFactory->CreateSession(nullptr, IID_IDBCreateCommand, (IUnknown**)commandFactory.GetAddressOf())) ||
        FAILED(commandFactory->CreateCommand(nullptr, IID_ICommandText, (IUnknown**)command.GetAddressOf())) ||
        FAILED(command->SetCommandText(DBGUID_DEFAULT, sql.c_str()))) {
        error = "could not set up the index query";
        return false;
    }
    ComPtr<IRowset> rowset;
    hr = command->Execute(nullptr, IID_IRowset, nullptr, nullptr, (IUnknown**)rowset.GetAddressOf());
    if (FAILED(hr) || !rowset) {
        char b[64];
        std::snprintf(b, sizeof(b), "the index query failed (0x%08lx)", (unsigned long)hr);
        error = b;
        return false;
    }
    ComPtr<IAccessor> accessor;
    if (FAILED(rowset.As(&accessor))) {
        error = "no accessor";
        return false;
    }
    std::vector<DBBINDING> bindings(columns);
    for (size_t i = 0; i < columns; ++i) {
        DBBINDING& b = bindings[i];
        b = {};
        b.iOrdinal = (DBORDINAL)(i + 1);
        b.obValue = i * sizeof(Cell) + offsetof(Cell, value);
        b.obStatus = i * sizeof(Cell) + offsetof(Cell, status);
        b.obLength = i * sizeof(Cell) + offsetof(Cell, length);
        b.dwPart = DBPART_VALUE | DBPART_STATUS | DBPART_LENGTH;
        b.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
        b.eParamIO = DBPARAMIO_NOTPARAM;
        b.cbMaxLen = sizeof(VARIANT);
        b.wType = DBTYPE_VARIANT;
    }
    HACCESSOR handle = 0;
    if (FAILED(accessor->CreateAccessor(DBACCESSOR_ROWDATA, columns, bindings.data(), columns * sizeof(Cell),
                                        &handle, nullptr))) {
        error = "could not bind the index columns";
        return false;
    }
    std::vector<Cell> cells(columns);
    HROW rows[256];
    for (;;) {
        DBCOUNTITEM got = 0;
        HROW* p = rows;
        hr = rowset->GetNextRows(DB_NULL_HCHAPTER, 0, 256, &got, &p);
        for (DBCOUNTITEM r = 0; r < got; ++r) {
            for (Cell& c : cells) VariantInit(&c.value);
            if (SUCCEEDED(rowset->GetData(rows[r], handle, cells.data()))) {
                for (Cell& c : cells)
                    if (c.status != DBSTATUS_S_OK) {
                        VariantClear(&c.value);
                        VariantInit(&c.value);
                    }
                row(cells);
            }
            for (Cell& c : cells) VariantClear(&c.value);
        }
        if (got) rowset->ReleaseRows(got, rows, nullptr, nullptr, nullptr);
        if (hr != S_OK || got == 0) break;  // DB_S_ENDOFROWSET or an error
    }
    accessor->ReleaseAccessor(handle, nullptr);
    return true;
}

std::wstring ScopeClause(const std::string& scope) {
    if (scope.empty()) return {};
    std::wstring s = Wide(scope);
    std::wstring quoted;
    for (wchar_t c : s) {
        if (c == L'\'') quoted += L"''";
        else quoted += c;
    }
    return L" AND SCOPE='file:" + quoted + L"'";
}

} // namespace

ScopedCom::ScopedCom() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    uninit_ = SUCCEEDED(hr);
}

ScopedCom::~ScopedCom() {
    if (uninit_) CoUninitialize();
}

bool IsCloudOnly(uint32_t a) {
    return (a & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) != 0;
}

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> LibraryFolders(Library library) {
    std::vector<std::string> out;
    const KNOWNFOLDERID& lib = library == Library::Pictures ? FOLDERID_PicturesLibrary : FOLDERID_VideosLibrary;
    ComPtr<IShellLibrary> shellLib;
    if (SUCCEEDED(SHLoadLibraryFromKnownFolder(lib, STGM_READ, IID_PPV_ARGS(&shellLib)))) {
        ComPtr<IShellItemArray> folders;
        if (SUCCEEDED(shellLib->GetFolders(LFF_FORCEFILESYSTEM, IID_PPV_ARGS(&folders)))) {
            DWORD n = 0;
            folders->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                ComPtr<IShellItem> item;
                PWSTR path = nullptr;
                if (SUCCEEDED(folders->GetItemAt(i, &item)) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out.push_back(Utf8(path, wcslen(path)));
                    CoTaskMemFree(path);
                }
            }
        }
    }
    if (out.empty()) {
        PWSTR path = nullptr;
        const KNOWNFOLDERID& known = library == Library::Pictures ? FOLDERID_Pictures : FOLDERID_Videos;
        if (SUCCEEDED(SHGetKnownFolderPath(known, KF_FLAG_DEFAULT, nullptr, &path)) && path)
            out.push_back(Utf8(path, wcslen(path)));
        CoTaskMemFree(path);
    }
    return out;
}

std::string DownloadsFolder() {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path)) && path)
        out = Utf8(path, wcslen(path));
    CoTaskMemFree(path);
    return out;
}

bool QueryIndex(const IndexQuery& q, std::vector<FoundFile>& out, std::string& error) {
    HiddenFolders hidden;
    const std::wstring scope = ScopeClause(q.scope);
    auto keep = [&](FoundFile& f, uint32_t attributes) {
        if (f.path.empty() || ExcludedPath(f.path)) return;
        SplitPath(f.path, f.folder, f.name);
        if (hidden.Hidden(f.folder)) return;
        f.cloudOnly = IsCloudOnly(attributes);
        out.push_back(std::move(f));
    };

    // Pictures. A picture the index has no size for is kept when it is big enough on
    // disk to be a photo (formats the indexer cannot read the header of).
    const std::wstring pictures =
        L"SELECT System.ItemPathDisplay, System.Size, System.DateModified, System.Photo.DateTaken, "
        L"System.Image.HorizontalSize, System.Image.VerticalSize, System.FileAttributes "
        L"FROM SystemIndex WHERE System.Kind = 'picture'" + scope;
    bool ok = RunQuery(pictures, 7, [&](const std::vector<Cell>& c) {
        FoundFile f;
        f.path = VarString(c[0].value);
        f.bytes = VarInt(c[1].value);
        const int64_t modified = VarDateMs(c[2].value);
        const int64_t taken = VarDateMs(c[3].value);
        f.modified = modified / 1000;
        f.dateMs = taken ? taken : modified;
        f.width = (uint32_t)VarInt(c[4].value);
        f.height = (uint32_t)VarInt(c[5].value);
        const uint32_t edge = std::max(f.width, f.height);
        if (edge ? edge < q.minPictureEdge : f.bytes < 256 * 1024) return;
        keep(f, (uint32_t)VarInt(c[6].value));
    }, error);
    if (!ok) return false;

    const std::wstring videos =
        L"SELECT System.ItemPathDisplay, System.Size, System.DateModified, System.Video.FrameWidth, "
        L"System.Video.FrameHeight, System.Media.Duration, System.FileAttributes "
        L"FROM SystemIndex WHERE System.Kind = 'video'" + scope;
    ok = RunQuery(videos, 7, [&](const std::vector<Cell>& c) {
        FoundFile f;
        f.video = true;
        f.path = VarString(c[0].value);
        f.bytes = VarInt(c[1].value);
        if (f.bytes < q.minVideoBytes) return;
        f.dateMs = VarDateMs(c[2].value);
        f.modified = f.dateMs / 1000;
        f.width = (uint32_t)VarInt(c[3].value);
        f.height = (uint32_t)VarInt(c[4].value);
        f.durationMs = VarInt(c[5].value) / 10000;  // 100 ns units
        keep(f, (uint32_t)VarInt(c[6].value));
    }, error);
    return ok;
}

bool WalkFolder(const std::string& folder, int maxDepth, double deadlineMs,
                bool (*accept)(const std::string& path, bool& video), std::vector<FoundFile>& out) {
    std::string base = folder;
    if (!base.empty() && base.back() != '\\' && base.back() != '/') base += '\\';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Wide(base + "*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool complete = true;
    std::vector<std::string> subfolders;
    do {
        if (deadlineMs > 0.0 && NowMs() > deadlineMs) {
            complete = false;
            break;
        }
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        const std::string name = Utf8(n, wcslen(n));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) subfolders.push_back(base + name);
            continue;
        }
        FoundFile f;
        f.path = base + name;
        if (!accept(f.path, f.video)) continue;
        f.folder = base;
        f.name = name;
        f.bytes = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        f.dateMs = FileTimeMs(fd.ftLastWriteTime);
        f.modified = f.dateMs / 1000;
        f.cloudOnly = IsCloudOnly(fd.dwFileAttributes);
        out.push_back(std::move(f));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (complete && maxDepth > 0)
        for (const std::string& sub : subfolders)
            if (!WalkFolder(sub, maxDepth - 1, deadlineMs, accept, out)) return false;
    return complete;
}

bool ShellThumbnail(const std::string& path, uint32_t maxEdge, bool cacheOnly, uint32_t& width,
                    uint32_t& height, std::vector<uint8_t>& rgba) {
    width = height = 0;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(Wide(path).c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    SIIGBF flags = SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT;
    if (cacheOnly) flags |= SIIGBF_INCACHEONLY;
    HBITMAP bmp = nullptr;
    const SIZE size{(LONG)maxEdge, (LONG)maxEdge};
    if (FAILED(factory->GetImage(size, flags, &bmp)) || !bmp) return false;
    BITMAP info{};
    bool ok = false;
    if (GetObjectW(bmp, sizeof(info), &info) && info.bmWidth > 0 && info.bmHeight != 0) {
        const int w = info.bmWidth, h = std::abs(info.bmHeight);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        rgba.resize((size_t)w * h * 4);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, bmp, 0, (UINT)h, rgba.data(), &bi, DIB_RGB_COLORS) == h;
        ReleaseDC(nullptr, dc);
        if (ok) {
            // BGRA -> RGBA. A bitmap with no alpha at all (most JPEG thumbnails) reads as
            // fully transparent: make it opaque.
            bool anyAlpha = false;
            for (size_t i = 3; i < rgba.size(); i += 4)
                if (rgba[i]) {
                    anyAlpha = true;
                    break;
                }
            for (size_t i = 0; i < rgba.size(); i += 4) {
                std::swap(rgba[i], rgba[i + 2]);
                if (!anyAlpha) rgba[i + 3] = 255;
            }
            width = (uint32_t)w;
            height = (uint32_t)h;
        }
    }
    DeleteObject(bmp);
    if (!ok) rgba.clear();
    return ok;
}

} // namespace mp::browser::win
