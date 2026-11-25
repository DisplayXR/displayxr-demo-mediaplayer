// SPDX-License-Identifier: Apache-2.0
//
// Internal: positional I/O on a POSIX file descriptor (macOS, Linux, Android), with a
// test hook that makes the Nth modifying operation fail, to prove that every
// intermediate state of an edit is a valid file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dxrmedia {
namespace detail {

// Tests only: when >= 0, the modifying operation (write, truncate, sync) with this
// 0-based index fails, and so does every later one, as if the process had died just
// before it. -1 = off. Counts across calls; reset it between scenarios.
extern long g_failAtOp;
extern long g_opCount;

class Fd {
public:
	explicit Fd(int fd) : fd_(fd) {}
	bool size(uint64_t& out) const;
	bool read(uint64_t off, void* buf, size_t n) const;  // exactly n bytes
	bool write(uint64_t off, const void* buf, size_t n);  // exactly n bytes
	bool truncate(uint64_t len);
	bool sync();  // F_FULLFSYNC on Apple, fsync elsewhere
	int fd() const { return fd_; }

private:
	bool tick();
	int fd_;
};

// Opens `path` (read-write when `write`), returns -1 and sets `error` on failure.
int open_path(const std::string& path, bool write, std::string* error);
void close_fd(int fd);

}  // namespace detail
}  // namespace dxrmedia
