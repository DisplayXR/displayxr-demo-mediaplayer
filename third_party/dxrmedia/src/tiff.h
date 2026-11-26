// SPDX-License-Identifier: Apache-2.0
//
// Internal: byte-order helpers and a tiny TIFF IFD writer/reader shared by the
// EXIF and MPF code. Everything that reads is bounds-checked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxrmedia {
namespace tiff {

enum Type : uint16_t {
	BYTE = 1, ASCII = 2, SHORT = 3, LONG = 4, RATIONAL = 5, UNDEFINED = 7, SLONG = 9, SRATIONAL = 10,
};

inline void put16(std::vector<uint8_t>& o, uint16_t v, bool be) {
	if (be) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
	else { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); }
}
inline void put32(std::vector<uint8_t>& o, uint32_t v, bool be) {
	if (be) { put16(o, (uint16_t)(v >> 16), true); put16(o, (uint16_t)v, true); }
	else { put16(o, (uint16_t)v, false); put16(o, (uint16_t)(v >> 16), false); }
}
inline void set32(std::vector<uint8_t>& o, size_t at, uint32_t v, bool be) {
	std::vector<uint8_t> t;
	put32(t, v, be);
	for (int i = 0; i < 4; ++i) o[at + i] = t[i];
}

struct Entry {
	uint16_t tag = 0;
	uint16_t type = 0;
	uint32_t count = 0;
	std::vector<uint8_t> data;  // already in the IFD's byte order
};

struct Builder {
	bool be = true;
	std::vector<Entry> entries;

	void ascii(uint16_t tag, const std::string& s);
	void undefined(uint16_t tag, const std::vector<uint8_t>& bytes);
	void u16(uint16_t tag, uint16_t v);
	void u32(uint16_t tag, uint32_t v);
	void rational(uint16_t tag, uint32_t num, uint32_t den);
	void srational(uint16_t tag, int32_t num, int32_t den);

	// Size of the IFD (count + entries + next pointer) plus its value area.
	size_t size() const;
	// Serialise at `ifdOffset` (relative to the TIFF header). Entries are sorted by
	// tag. If `patchTag` is non-zero, `*patchAt` receives the offset (into `out`, which
	// the bytes are appended to) of that entry's 4-byte value field.
	void write(std::vector<uint8_t>& out, uint32_t ifdOffset, uint32_t nextIfd,
	           uint16_t patchTag = 0, size_t* patchAt = nullptr) const;
};

// Positive real -> rational (1/n for short exposures, else a decimal fraction).
void to_rational(double x, uint32_t& num, uint32_t& den);

// Reader over a TIFF block [b, b+n). All offsets relative to b.
struct Reader {
	const uint8_t* b = nullptr;
	size_t n = 0;
	bool be = true;

	bool header(uint32_t& firstIfd);  // byte order + magic 42
	bool u16(size_t at, uint16_t& v) const;
	bool u32(size_t at, uint32_t& v) const;

	struct Field {
		uint16_t tag = 0, type = 0;
		uint32_t count = 0;
		size_t valueAt = 0;  // where the value bytes are (inline or at the offset)
		size_t valueSize = 0;
	};
	// Reads one IFD; false if any part lies outside the block. `next` = next-IFD offset.
	bool ifd(uint32_t at, std::vector<Field>& fields, uint32_t& next) const;

	static const Field* find(const std::vector<Field>& f, uint16_t tag);
	bool asU32(const Field& f, uint32_t& v) const;          // SHORT or LONG, first value
	bool asRational(const Field& f, double& v) const;       // RATIONAL / SRATIONAL
	std::string asString(const Field& f) const;             // ASCII, NUL-trimmed
	std::vector<uint8_t> asBytes(const Field& f) const;
};

size_t type_size(uint16_t type);

}  // namespace tiff
}  // namespace dxrmedia
