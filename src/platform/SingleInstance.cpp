// SPDX-License-Identifier: Apache-2.0
#include "platform/SingleInstance.h"

#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mp::single_instance {

#if defined(_WIN32)
namespace {

constexpr ULONG_PTR kOpenTag = 0x44585250;  // 'DXRP': a forwarded launch
const wchar_t* gClass = L"DisplayXRMediaPlayer.Instance";

std::mutex gMutex;
std::vector<std::string> gPending;
bool gHavePending = false;

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cd && cd->dwData == kOpenTag) {
            std::string path(static_cast<const char*>(cd->lpData), cd->cbData);
            std::lock_guard<std::mutex> lock(gMutex);
            gPending.push_back(std::move(path));
            gHavePending = true;
            return TRUE;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool MultiAllowed() {
    const char* e = std::getenv("MEDIAPLAYER_MULTI_INSTANCE");
    return e && *e && *e != '0';
}

}  // namespace

bool Claim(bool pro) {
    if (MultiAllowed()) return true;
    // Held for the life of the process (never closed: the OS drops it at exit).
    CreateMutexW(nullptr, FALSE, pro ? L"Local\\DisplayXRMediaPlayerPro.Instance" : L"Local\\DisplayXRMediaPlayer.Instance");
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

bool Forward(const std::string& path, int waitMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        if (HWND target = FindWindowExW(HWND_MESSAGE, nullptr, gClass, nullptr)) {
            // Let the running player bring its window to the front (only the foreground
            // process, this one, can grant that).
            DWORD pid = 0;
            GetWindowThreadProcessId(target, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            COPYDATASTRUCT cd{};
            cd.dwData = kOpenTag;
            cd.cbData = (DWORD)path.size();
            cd.lpData = (void*)path.data();
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &result) && result)
                return true;
        }
        if ((int)(GetTickCount() - start) >= waitMs) return false;
        Sleep(100);
    }
}

void Listen() {
    if (MultiAllowed()) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = gClass;
    RegisterClassExW(&wc);
    // A message-only window, pumped by SDL's event loop on this thread.
    CreateWindowExW(0, gClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

bool Take(std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gHavePending) return false;
    paths.swap(gPending);
    gPending.clear();
    gHavePending = false;
    return true;
}

#else

bool Claim(bool) { return true; }
bool Forward(const std::string&, int) { return false; }
void Listen() {}
bool Take(std::vector<std::string>&) { return false; }

#endif

}  // namespace mp::single_instance
