// SPDX-License-Identifier: Apache-2.0
#include "fileio.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dxrmedia {
namespace detail {

long g_failAtOp = -1;
long g_opCount = 0;

bool Fd::tick() {
	const long i = g_opCount++;
	return !(g_failAtOp >= 0 && i >= g_failAtOp);
}

bool Fd::size(uint64_t& out) const {
	struct stat st;
	if (fstat(fd_, &st) != 0 || st.st_size < 0) return false;
	out = (uint64_t)st.st_size;
	return true;
}

bool Fd::read(uint64_t off, void* buf, size_t n) const {
	uint8_t* p = static_cast<uint8_t*>(buf);
	while (n > 0) {
		const ssize_t r = pread(fd_, p, n, (off_t)off);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return false;
		p += r;
		off += (uint64_t)r;
		n -= (size_t)r;
	}
	return true;
}

bool Fd::write(uint64_t off, const void* buf, size_t n) {
	if (!tick()) return false;
	const uint8_t* p = static_cast<const uint8_t*>(buf);
	while (n > 0) {
		const ssize_t r = pwrite(fd_, p, n, (off_t)off);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return false;
		p += r;
		off += (uint64_t)r;
		n -= (size_t)r;
	}
	return true;
}

bool Fd::truncate(uint64_t len) {
	if (!tick()) return false;
	return ftruncate(fd_, (off_t)len) == 0;
}

bool Fd::sync() {
	if (!tick()) return false;
#if defined(__APPLE__) && defined(F_FULLFSYNC)
	if (fcntl(fd_, F_FULLFSYNC) == 0) return true;  // falls back when the FS refuses
#endif
	return fsync(fd_) == 0;
}

int open_path(const std::string& path, bool write, std::string* error) {
	int fd;
	do {
		fd = ::open(path.c_str(), (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0 && error) *error = "cannot open " + path + ": " + std::strerror(errno);
	return fd;
}

void close_fd(int fd) {
	if (fd >= 0) ::close(fd);
}

}  // namespace detail
}  // namespace dxrmedia
