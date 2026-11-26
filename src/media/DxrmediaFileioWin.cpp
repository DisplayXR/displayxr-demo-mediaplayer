// SPDX-License-Identifier: Apache-2.0
//
// Windows build of dxrmedia's positional-I/O helper (third_party/dxrmedia/src/fileio.h).
// The vendored fileio.cpp is POSIX (pread / pwrite / fsync); MSVC has none of them, so
// on Windows this file is compiled in its place and the vendored copy stays unmodified.
// Same contract: positional reads and writes that do not depend on the descriptor's
// file offset, and the test hook counters. Paths are UTF-8 (the player's convention,
// windows/utf8.manifest) and are widened for _wopen.
#include "fileio.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace dxrmedia {
namespace detail {

long g_failAtOp = -1;
long g_opCount = 0;

namespace {

HANDLE HandleOf(int fd) { return reinterpret_cast<HANDLE>(_get_osfhandle(fd)); }

OVERLAPPED At(uint64_t off) {
	OVERLAPPED ov = {};
	ov.Offset = (DWORD)(off & 0xFFFFFFFFull);
	ov.OffsetHigh = (DWORD)(off >> 32);
	return ov;
}

}  // namespace

bool Fd::tick() {
	const long i = g_opCount++;
	return !(g_failAtOp >= 0 && i >= g_failAtOp);
}

bool Fd::size(uint64_t& out) const {
	struct _stat64 st;
	if (_fstat64(fd_, &st) != 0 || st.st_size < 0) return false;
	out = (uint64_t)st.st_size;
	return true;
}

bool Fd::read(uint64_t off, void* buf, size_t n) const {
	const HANDLE h = HandleOf(fd_);
	if (h == INVALID_HANDLE_VALUE) return false;
	uint8_t* p = static_cast<uint8_t*>(buf);
	while (n > 0) {
		const DWORD want = n > 0x40000000u ? 0x40000000u : (DWORD)n;
		OVERLAPPED ov = At(off);
		DWORD got = 0;
		if (!ReadFile(h, p, want, &got, &ov) || got == 0) return false;
		p += got;
		off += got;
		n -= got;
	}
	return true;
}

bool Fd::write(uint64_t off, const void* buf, size_t n) {
	if (!tick()) return false;
	const HANDLE h = HandleOf(fd_);
	if (h == INVALID_HANDLE_VALUE) return false;
	const uint8_t* p = static_cast<const uint8_t*>(buf);
	while (n > 0) {
		const DWORD want = n > 0x40000000u ? 0x40000000u : (DWORD)n;
		OVERLAPPED ov = At(off);
		DWORD put = 0;
		if (!WriteFile(h, p, want, &put, &ov) || put == 0) return false;
		p += put;
		off += put;
		n -= put;
	}
	return true;
}

bool Fd::truncate(uint64_t len) {
	if (!tick()) return false;
	return _chsize_s(fd_, (__int64)len) == 0;
}

bool Fd::sync() {
	if (!tick()) return false;
	return _commit(fd_) == 0;
}

int open_path(const std::string& path, bool write, std::string* error) {
	std::wstring w;
	const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
	if (n > 0) {
		w.resize((size_t)n);
		MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &w[0], n);
	}
	const int flags = (write ? _O_RDWR : _O_RDONLY) | _O_BINARY | _O_NOINHERIT;
	const int fd = n > 0 ? _wopen(w.c_str(), flags) : -1;
	if (fd < 0 && error) *error = "cannot open " + path + ": " + std::strerror(errno);
	return fd;
}

void close_fd(int fd) {
	if (fd >= 0) _close(fd);
}

}  // namespace detail
}  // namespace dxrmedia
