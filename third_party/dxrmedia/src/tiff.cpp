// SPDX-License-Identifier: Apache-2.0
#include "tiff.h"

#include <algorithm>
#include <cmath>

namespace dxrmedia {
namespace tiff {

size_t type_size(uint16_t type) {
	switch (type) {
	case BYTE: case ASCII: case UNDEFINED: return 1;
	case SHORT: return 2;
	case LONG: case SLONG: return 4;
	case RATIONAL: case SRATIONAL: return 8;
	default: return 0;
	}
}

void Builder::ascii(uint16_t tag, const std::string& s) {
	Entry e;
	e.tag = tag;
	e.type = ASCII;
	e.data.assign(s.begin(), s.end());
	e.data.push_back(0);
	e.count = (uint32_t)e.data.size();
	entries.push_back(std::move(e));
}

void Builder::undefined(uint16_t tag, const std::vector<uint8_t>& bytes) {
	Entry e;
	e.tag = tag;
	e.type = UNDEFINED;
	e.data = bytes;
	e.count = (uint32_t)bytes.size();
	entries.push_back(std::move(e));
}

void Builder::u16(uint16_t tag, uint16_t v) {
	Entry e;
	e.tag = tag;
	e.type = SHORT;
	e.count = 1;
	put16(e.data, v, be);
	entries.push_back(std::move(e));
}

void Builder::u32(uint16_t tag, uint32_t v) {
	Entry e;
	e.tag = tag;
	e.type = LONG;
	e.count = 1;
	put32(e.data, v, be);
	entries.push_back(std::move(e));
}

void Builder::rational(uint16_t tag, uint32_t num, uint32_t den) {
	Entry e;
	e.tag = tag;
	e.type = RATIONAL;
	e.count = 1;
	put32(e.data, num, be);
	put32(e.data, den, be);
	entries.push_back(std::move(e));
}

void Builder::srational(uint16_t tag, int32_t num, int32_t den) {
	Entry e;
	e.tag = tag;
	e.type = SRATIONAL;
	e.count = 1;
	put32(e.data, (uint32_t)num, be);
	put32(e.data, (uint32_t)den, be);
	entries.push_back(std::move(e));
}

size_t Builder::size() const {
	size_t s = 2 + entries.size() * 12 + 4;
	for (const auto& e : entries)
		if (e.data.size() > 4) s += (e.data.size() + 1) & ~(size_t)1;
	return s;
}

void Builder::write(std::vector<uint8_t>& out, uint32_t ifdOffset, uint32_t nextIfd,
                    uint16_t patchTag, size_t* patchAt) const {
	std::vector<const Entry*> sorted;
	for (const auto& e : entries) sorted.push_back(&e);
	std::stable_sort(sorted.begin(), sorted.end(),
	                 [](const Entry* a, const Entry* b) { return a->tag < b->tag; });

	put16(out, (uint16_t)sorted.size(), be);
	uint32_t valueOff = ifdOffset + 2 + (uint32_t)sorted.size() * 12 + 4;
	std::vector<uint8_t> values;
	for (const Entry* e : sorted) {
		put16(out, e->tag, be);
		put16(out, e->type, be);
		put32(out, e->count, be);
		if (e->tag == patchTag && patchAt) *patchAt = out.size();
		if (e->data.size() <= 4) {
			std::vector<uint8_t> v = e->data;
			v.resize(4, 0);
			out.insert(out.end(), v.begin(), v.end());
		} else {
			put32(out, valueOff + (uint32_t)values.size(), be);
			values.insert(values.end(), e->data.begin(), e->data.end());
			if (values.size() & 1) values.push_back(0);
		}
	}
	put32(out, nextIfd, be);
	out.insert(out.end(), values.begin(), values.end());
}

void to_rational(double x, uint32_t& num, uint32_t& den) {
	if (!(x > 0) || !std::isfinite(x)) { num = 0; den = 1; return; }
	if (x < 1) {
		const double n = std::round(1.0 / x);
		if (n >= 1 && n < 4.0e9 && std::fabs(1.0 / n - x) <= 1e-6 * x) {
			num = 1;
			den = (uint32_t)n;
			return;
		}
	}
	uint64_t d = 1000000;
	while (d > 1 && x * (double)d > 4.0e9) d /= 10;
	if (x * (double)d > 4.0e9) { num = 0xFFFFFFFFu; den = 1; return; }
	uint64_t n = (uint64_t)std::llround(x * (double)d);
	uint64_t a = n, b = d;
	while (b) { const uint64_t t = a % b; a = b; b = t; }
	if (a > 1) { n /= a; d /= a; }
	num = (uint32_t)n;
	den = (uint32_t)d;
}

bool Reader::u16(size_t at, uint16_t& v) const {
	if (at > n || n - at < 2) return false;
	const uint8_t* p = b + at;
	v = be ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8));
	return true;
}

bool Reader::u32(size_t at, uint32_t& v) const {
	if (at > n || n - at < 4) return false;
	const uint8_t* p = b + at;
	v = be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
	       : (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	return true;
}

bool Reader::header(uint32_t& firstIfd) {
	if (n < 8) return false;
	if (b[0] == 'M' && b[1] == 'M') be = true;
	else if (b[0] == 'I' && b[1] == 'I') be = false;
	else return false;
	uint16_t magic = 0;
	if (!u16(2, magic) || magic != 42) return false;
	return u32(4, firstIfd);
}

bool Reader::ifd(uint32_t at, std::vector<Field>& fields, uint32_t& next) const {
	fields.clear();
	uint16_t count = 0;
	if (!u16(at, count)) return false;
	const size_t end = (size_t)at + 2 + (size_t)count * 12;
	if (end + 4 > n) return false;
	for (uint16_t i = 0; i < count; ++i) {
		const size_t e = (size_t)at + 2 + (size_t)i * 12;
		Field f;
		uint32_t off = 0;
		if (!u16(e, f.tag) || !u16(e + 2, f.type) || !u32(e + 4, f.count) || !u32(e + 8, off))
			return false;
		const size_t ts = type_size(f.type);
		if (ts == 0) continue;  // unknown type: skip, it cannot be sized
		if (f.count > n / ts) return false;
		f.valueSize = ts * f.count;
		f.valueAt = f.valueSize <= 4 ? e + 8 : off;
		if (f.valueAt > n || n - f.valueAt < f.valueSize) return false;
		fields.push_back(f);
	}
	return u32(end, next);
}

const Reader::Field* Reader::find(const std::vector<Field>& f, uint16_t tag) {
	for (const auto& x : f)
		if (x.tag == tag) return &x;
	return nullptr;
}

bool Reader::asU32(const Field& f, uint32_t& v) const {
	if (f.count < 1) return false;
	if (f.type == SHORT) {
		uint16_t s = 0;
		if (!u16(f.valueAt, s)) return false;
		v = s;
		return true;
	}
	if (f.type == LONG || f.type == SLONG) return u32(f.valueAt, v);
	return false;
}

bool Reader::asRational(const Field& f, double& v) const {
	if (f.count < 1 || (f.type != RATIONAL && f.type != SRATIONAL)) return false;
	uint32_t a = 0, d = 0;
	if (!u32(f.valueAt, a) || !u32(f.valueAt + 4, d) || d == 0) return false;
	v = f.type == RATIONAL ? (double)a / (double)d : (double)(int32_t)a / (double)(int32_t)d;
	return std::isfinite(v);
}

std::string Reader::asString(const Field& f) const {
	if (f.type != ASCII && f.type != UNDEFINED && f.type != BYTE) return std::string();
	std::string s(reinterpret_cast<const char*>(b + f.valueAt), f.valueSize);
	while (!s.empty() && s.back() == '\0') s.pop_back();
	const size_t z = s.find('\0');
	if (z != std::string::npos) s.resize(z);
	return s;
}

std::vector<uint8_t> Reader::asBytes(const Field& f) const {
	return std::vector<uint8_t>(b + f.valueAt, b + f.valueAt + f.valueSize);
}

}  // namespace tiff
}  // namespace dxrmedia
