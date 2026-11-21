// SPDX-License-Identifier: Apache-2.0
//
// JoinCancellingIo - join a worker that may be stuck in a blocking file read (#251).
//
// A file on an SMB share opens through FFmpeg's plain file protocol (and the shell / Win32
// file calls), so a read the network has stalled blocks in a synchronous ReadFile that no
// stop flag or AVIOInterruptCB can reach: Stop() joined such a thread, and closing the
// player hung with its window frozen. On Windows this cancels the thread's pending
// synchronous I/O (CancelSynchronousIo) until it exits; the read fails, the worker sees
// its stop flag and returns. The caller sets that flag first. A plain join elsewhere.
//
// Header-only; include it from .cpp files only (it pulls in windows.h).
#pragma once

#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mp {

inline void JoinCancellingIo(std::thread& t) {
    if (!t.joinable()) return;
#if defined(_WIN32)
    const HANDLE h = (HANDLE)t.native_handle();
    // Repeated: a cancel only hits the read pending at that moment, and the worker may
    // start another before it next checks its flag.
    while (WaitForSingleObject(h, 50) == WAIT_TIMEOUT) CancelSynchronousIo(h);
#endif
    t.join();
}

}  // namespace mp
