// SPDX-License-Identifier: Apache-2.0
#include "dxrmedia/video.h"

#include "dxrmedia/xmp.h"
#include "fileio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dxrmedia {

const uint8_t kXmpUuid[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                              0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};

// ------------------------------------------------------------ timed metadata

namespace {

void le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void lef(uint8_t* p, float f) {
	uint32_t u;
	std::memcpy(&u, &f, 4);
	le32(p, u);
}
float rdf(const uint8_t* p) {
	const uint32_t u = rd32(p);
	float f;
	std::memcpy(&f, &u, 4);
	return f;
}

}  // namespace

std::array<uint8_t, kFrameSampleSize> encode_frame_meta(const FrameMeta& m) {
	// A bit is set only when its value is real: a non-finite value clears it.
	const bool conv = m.convergenceValid && std::isfinite(m.convergence);
	const bool dist = m.convergenceDistanceValid && std::isfinite(m.convergenceDistanceMm);
	const bool range = m.disparityRangeValid && std::isfinite(m.disparityNear) && std::isfinite(m.disparityFar);
	const bool focus = m.focusValid && std::isfinite(m.focusDistanceMm);
	const bool expo = m.exposureValid && std::isfinite(m.exposureMs);
	std::array<uint8_t, kFrameSampleSize> o{};
	o[0] = 1;
	o[1] = conv ? (uint8_t)m.convergenceSource : 0;  // meaningful only with bit 0
	uint16_t flags = 0;
	if (conv) flags |= kFlagConvergenceValid;
	if (focus) flags |= kFlagFocusValid;
	if (m.sceneCut) flags |= kFlagSceneCut;
	if (range) flags |= kFlagDisparityRangeValid;
	if (expo) flags |= kFlagExposureValid;
	if (dist) flags |= kFlagConvergenceDistanceValid;
	le16(&o[2], flags);
	// A field whose bit is clear is written 0 (FORMAT.md 3.3).
	lef(&o[4], conv ? m.convergence : 0.f);
	lef(&o[8], dist ? m.convergenceDistanceMm : 0.f);
	lef(&o[12], range ? m.disparityNear : 0.f);
	lef(&o[16], range ? m.disparityFar : 0.f);
	lef(&o[20], focus ? m.focusDistanceMm : 0.f);
	lef(&o[24], expo ? m.exposureMs : 0.f);
	le16(&o[28], expo ? m.iso : 0);
	le16(&o[30], (uint16_t)m.rightMinusLeftUs);
	return o;
}

bool decode_frame_meta(const uint8_t* p, size_t n, FrameMeta& m) {
	if (!p || n < kFrameSampleSize || p[0] < 1) return false;
	m = FrameMeta();
	m.version = p[0];
	m.flags = rd16(p + 2);
	m.sceneCut = (m.flags & kFlagSceneCut) != 0;
	// A reader ignores a field whose bit is clear, and treats a non-finite stored
	// value as not valid (FORMAT.md 3.3): such fields come back 0 / false.
	if (m.flags & kFlagConvergenceValid) {
		const float v = rdf(p + 4);
		if (std::isfinite(v)) {
			m.convergenceValid = true;
			m.convergence = v;
			m.convergenceSource = (ConvergenceSource)p[1];
		}
	}
	if (m.flags & kFlagConvergenceDistanceValid) {
		const float v = rdf(p + 8);
		if (std::isfinite(v)) { m.convergenceDistanceValid = true; m.convergenceDistanceMm = v; }
	}
	if (m.flags & kFlagDisparityRangeValid) {
		const float a = rdf(p + 12), b = rdf(p + 16);
		if (std::isfinite(a) && std::isfinite(b)) {
			m.disparityRangeValid = true;
			m.disparityNear = a;
			m.disparityFar = b;
		}
	}
	if (m.flags & kFlagFocusValid) {
		const float v = rdf(p + 20);
		if (std::isfinite(v)) { m.focusValid = true; m.focusDistanceMm = v; }
	}
	if (m.flags & kFlagExposureValid) {
		const float v = rdf(p + 24);
		if (std::isfinite(v)) {
			m.exposureValid = true;
			m.exposureMs = v;
			m.iso = rd16(p + 28);
		}
	}
	m.rightMinusLeftUs = (int16_t)rd16(p + 30);
	return true;
}

int16_t saturate_delta_us(int64_t ns) {
	// Round to nearest microsecond (halves away from zero), then clamp to i16.
	// No negation of ns (INT64_MIN), no overflow from the rounding term.
	int64_t us = ns / 1000;
	const int64_t rem = ns % 1000;  // same sign as ns
	if (rem >= 500) ++us;
	else if (rem <= -500) --us;
	if (us > INT16_MAX) return INT16_MAX;
	if (us < INT16_MIN) return INT16_MIN;
	return (int16_t)us;
}

// ------------------------------------------------------------ MP4 helpers

namespace {

using detail::Fd;

void fail(std::string* e, const std::string& m) {
	if (e) *e = m;
}

uint32_t be32(const uint8_t* p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
void put_be32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
void put_be64(uint8_t* p, uint64_t v) {
	put_be32(p, (uint32_t)(v >> 32));
	put_be32(p + 4, (uint32_t)v);
}

struct Box {
	uint64_t pos = 0;    // header start
	uint64_t size = 0;   // whole box
	uint32_t hdr = 8;    // header bytes (8, 16 with largesize, +16 for uuid)
	char type[5] = {0, 0, 0, 0, 0};
	uint8_t uuid[16] = {0};
	bool largesize = false;
	bool toEnd = false;  // size field was 0
	uint64_t end() const { return pos + size; }
	uint64_t body() const { return pos + hdr; }
	bool is(const char* t) const { return std::memcmp(type, t, 4) == 0; }
	bool isFree() const { return is("free") || is("skip"); }
	bool isXmp() const { return is("uuid") && std::memcmp(uuid, kXmpUuid, 16) == 0; }
};

// Parse a box header from memory [p, p+avail) located at absolute position `pos`.
bool parseHeader(const uint8_t* p, uint64_t avail, uint64_t pos, uint64_t limit, Box& b) {
	if (avail < 8) return false;
	b = Box();
	b.pos = pos;
	uint64_t size = be32(p);
	std::memcpy(b.type, p + 4, 4);
	b.hdr = 8;
	if (size == 1) {
		if (avail < 16) return false;
		size = be64(p + 8);
		b.hdr = 16;
		b.largesize = true;
	} else if (size == 0) {
		size = limit - pos;
		b.toEnd = true;
	}
	if (b.is("uuid")) {
		if (avail < (uint64_t)b.hdr + 16) return false;
		std::memcpy(b.uuid, p + b.hdr, 16);
		b.hdr += 16;
	}
	if (size < b.hdr || size > limit - pos) return false;
	b.size = size;
	return true;
}

struct TopLevel {
	uint64_t fileSize = 0;
	uint64_t logicalEnd = 0;  // end of the last box; < 8 trailing bytes may follow
	std::vector<Box> boxes;
};

bool topLevel(const Fd& f, TopLevel& t, std::string* error) {
	t = TopLevel();
	if (!f.size(t.fileSize)) { fail(error, "cannot stat file"); return false; }
	uint64_t pos = 0;
	while (pos < t.fileSize) {
		if (t.fileSize - pos < 8) break;  // trailing bytes shorter than a header: ignored
		uint8_t h[32];
		const uint64_t want = std::min<uint64_t>(32, t.fileSize - pos);
		if (!f.read(pos, h, (size_t)want)) { fail(error, "read error"); return false; }
		Box b;
		if (!parseHeader(h, want, pos, t.fileSize, b)) {
			fail(error, "broken top-level box at offset " + std::to_string(pos) + " (truncated file?)");
			return false;
		}
		t.boxes.push_back(b);
		pos = b.end();
		if (t.boxes.size() > 100000) { fail(error, "too many boxes"); return false; }
	}
	t.logicalEnd = pos;
	if (t.boxes.empty()) { fail(error, "not an MP4 (no boxes)"); return false; }
	return true;
}

// In-memory tree over a buffer whose byte 0 is at absolute file position `base`.
struct Mem {
	const std::vector<uint8_t>& b;
	uint64_t base;
	// Children of [from, to). With `tailOk`, up to 7 trailing bytes after the last
	// child are allowed (QuickTime's 4-byte zero terminator in sample entries);
	// `*childEnd` receives the end of the last child.
	bool children(uint64_t from, uint64_t to, std::vector<Box>& out, bool tailOk = false,
	              uint64_t* childEnd = nullptr) const {
		out.clear();
		uint64_t pos = from;
		while (pos + 8 <= to) {
			Box c;
			const uint64_t rel = pos - base;
			if (!parseHeader(b.data() + rel, b.size() - rel, pos, to, c)) {
				if (tailOk && to - pos < 8) break;
				return false;
			}
			if (c.toEnd) return false;  // only legal at top level
			out.push_back(c);
			pos = c.end();
			if (out.size() > 100000) return false;
		}
		if (childEnd) *childEnd = pos;
		return pos == to || (tailOk && to - pos < 8);
	}
	const uint8_t* at(uint64_t abs) const { return b.data() + (abs - base); }
	static const Box* find(const std::vector<Box>& v, const char* t) {
		for (const auto& x : v)
			if (x.is(t)) return &x;
		return nullptr;
	}
};

constexpr uint64_t kMaxMoov = 256ull << 20;
constexpr uint32_t kVisualEntryFields = 78;  // SampleEntry 8 + VisualSampleEntry 70

struct SampleEntryRef {
	Box box;
	uint64_t childEnd = 0;  // where a new child goes (before any terminator)
	bool visual = false;
	bool hasSt3d = false;
};

struct TrackRef {
	Mp4Track info;
	std::vector<Box> path;  // moov, trak, mdia, minf, stbl, stsd
	std::vector<SampleEntryRef> entries;
	Box stbl;
	bool hasStbl = false;
};

bool parseTracks(const Mem& mem, const Box& moov, std::vector<TrackRef>& tracks, std::string* error) {
	tracks.clear();
	std::vector<Box> top;
	if (!mem.children(moov.body(), moov.end(), top)) { fail(error, "broken moov"); return false; }
	for (const Box& trak : top) {
		if (!trak.is("trak")) continue;
		TrackRef t;
		std::vector<Box> tk;
		if (!mem.children(trak.body(), trak.end(), tk)) { fail(error, "broken trak"); return false; }
		if (const Box* tkhd = Mem::find(tk, "tkhd")) {
			const uint8_t* p = mem.at(tkhd->body());
			const uint64_t n = tkhd->size - tkhd->hdr;
			const uint64_t idAt = n >= 1 && p[0] == 1 ? 20 : 12;
			if (n >= idAt + 4) t.info.trackId = be32(p + idAt);
		}
		const Box* mdia = Mem::find(tk, "mdia");
		if (!mdia) { tracks.push_back(t); continue; }
		std::vector<Box> md;
		if (!mem.children(mdia->body(), mdia->end(), md)) { fail(error, "broken mdia"); return false; }
		if (const Box* hdlr = Mem::find(md, "hdlr"); hdlr && hdlr->size - hdlr->hdr >= 12)
			t.info.handler.assign(reinterpret_cast<const char*>(mem.at(hdlr->body() + 8)), 4);
		const Box* minf = Mem::find(md, "minf");
		std::vector<Box> mi, st;
		if (minf && mem.children(minf->body(), minf->end(), mi)) {
			if (const Box* stbl = Mem::find(mi, "stbl"); stbl && mem.children(stbl->body(), stbl->end(), st)) {
				t.stbl = *stbl;
				t.hasStbl = true;
				if (const Box* stsd = Mem::find(st, "stsd"); stsd && stsd->size - stsd->hdr >= 8) {
					t.path = {moov, trak, *mdia, *minf, *stbl, *stsd};
					std::vector<Box> es;
					if (!mem.children(stsd->body() + 8, stsd->end(), es)) {
						fail(error, "broken stsd");
						return false;
					}
					for (const Box& e : es) {
						SampleEntryRef r;
						r.box = e;
						r.childEnd = e.end();
						if (t.info.handler == "vide" && e.size >= (uint64_t)e.hdr + kVisualEntryFields) {
							std::vector<Box> ec;
							if (!mem.children(e.body() + kVisualEntryFields, e.end(), ec, true, &r.childEnd)) {
								fail(error, "broken visual sample entry");
								return false;
							}
							r.visual = true;
							if (const Box* s3 = Mem::find(ec, "st3d")) {
								r.hasSt3d = true;
								if (s3->size - s3->hdr >= 5 && t.entries.empty()) {
									t.info.hasSt3d = true;
									t.info.st3dMode = mem.at(s3->body())[4];
								}
							}
						}
						t.entries.push_back(r);
					}
				}
				if (const Box* stsz = Mem::find(st, "stsz"); stsz && stsz->size - stsz->hdr >= 12)
					t.info.sampleCount = be32(mem.at(stsz->body() + 8));
			}
		}
		if (!t.entries.empty()) {
			const Box& e = t.entries[0].box;
			t.info.sampleEntry.assign(e.type, 4);
			if (e.is("mett") && e.size > (uint64_t)e.hdr + 8) {
				// SampleEntry (6 reserved + 2 index), content_encoding\0, mime_format\0
				const char* p = reinterpret_cast<const char*>(mem.at(e.body() + 8));
				const size_t n = (size_t)(e.size - e.hdr - 8);
				const size_t z1 = strnlen(p, n);
				if (z1 < n) t.info.mime.assign(p + z1 + 1, strnlen(p + z1 + 1, n - z1 - 1));
			}
		}
		tracks.push_back(t);
	}
	return true;
}

// The first moov is the one in charge (as with common demuxers); `extra` receives
// the positions of any later ones (left by an interrupted edit).
bool loadMoov(const Fd& f, const TopLevel& t, size_t& index, std::vector<uint8_t>& buf, std::string* error,
              std::vector<uint64_t>* extra = nullptr) {
	bool found = false;
	for (size_t i = 0; i < t.boxes.size(); ++i)
		if (t.boxes[i].is("moov")) {
			if (found) {
				if (extra) extra->push_back(t.boxes[i].pos);
				continue;
			}
			found = true;
			index = i;
		}
	if (!found) { fail(error, "no moov box"); return false; }
	const Box& m = t.boxes[index];
	if (m.size > kMaxMoov) { fail(error, "moov too large"); return false; }
	buf.resize((size_t)m.size);
	if (!f.read(m.pos, buf.data(), buf.size())) { fail(error, "read error"); return false; }
	if (m.toEnd) put_be32(buf.data(), m.size <= 0xFFFFFFFFull ? (uint32_t)m.size : 0);  // normalised copy
	return true;
}

// ------------------------------------------------------------ the finaliser

struct Plan {
	TopLevel top;
	size_t moovIndex = 0;
	std::vector<uint8_t> moov;         // the current moov bytes
	std::vector<uint8_t> newMoov;      // the moov to write (empty: moov stays as it is)
	int st3dAdded = 0;
	std::vector<uint8_t> xmpBox;       // empty: no XMP box to append
	bool xmpRequested = false;         // xmpBox is the caller's (not a carried-over copy)
	bool normaliseLast = false;        // the last box has size 0
	bool moovFirst = false;            // moov precedes the first mdat
	uint64_t regionPos = 0;            // a run of moov/free/skip boxes before the first mdat...
	uint64_t region = 0;               // ...of this many bytes, where the new moov will live
	bool regionLargesize = false;      // its first box has a 64-bit size
	bool returnToFront = false;        // put the new moov into that region
	std::vector<uint64_t> extraMoovs;  // later moov boxes (an interrupted earlier edit)
};

// Build the new moov: st3d appended as the last child of every visual sample entry
// of every video track that has none. `pad` extra zero bytes go into the first st3d
// (a reader skips a box's unread tail by its size).
bool buildSt3d(const std::vector<uint8_t>& moov, uint64_t base, const std::vector<TrackRef>& tracks, uint32_t pad,
               std::vector<uint8_t>& out, int& added, std::string* error) {
	struct Ins {
		uint64_t at;
		std::vector<Box> ancestors;
	};
	std::vector<Ins> ins;
	bool anyVideo = false;
	for (const TrackRef& t : tracks) {
		if (t.info.handler != "vide") continue;
		anyVideo = true;
		for (const SampleEntryRef& e : t.entries) {
			if (!e.visual || e.hasSt3d) continue;
			Ins i;
			i.at = e.childEnd;
			i.ancestors = t.path;
			i.ancestors.push_back(e.box);
			ins.push_back(i);
		}
	}
	if (!anyVideo) { fail(error, "no video track"); return false; }
	added = (int)ins.size();
	out.clear();
	if (ins.empty()) return true;
	std::sort(ins.begin(), ins.end(), [](const Ins& a, const Ins& b) { return a.at > b.at; });
	out = moov;
	for (size_t k = 0; k < ins.size(); ++k) {
		const uint32_t sz = 13 + (k + 1 == ins.size() ? pad : 0);  // last processed = first in file
		std::vector<uint8_t> st3d(sz, 0);
		put_be32(st3d.data(), sz);
		std::memcpy(st3d.data() + 4, "st3d", 4);
		st3d[12] = 2;  // stereo_mode: left-right
		for (const Box& a : ins[k].ancestors) {
			uint8_t* p = out.data() + (a.pos - base);
			if (a.largesize) {
				put_be64(p + 8, be64(p + 8) + sz);
			} else {
				const uint64_t s = (uint64_t)be32(p) + sz;
				if (s > 0xFFFFFFFFull) { fail(error, "box size overflow"); return false; }
				put_be32(p, (uint32_t)s);
			}
		}
		out.insert(out.begin() + (std::ptrdiff_t)(ins[k].at - base), st3d.begin(), st3d.end());
	}
	return true;
}

// A region of `region` bytes can hold a box of `n` bytes when it fits exactly, leaves
// room for a free header, or (with `canPad`) leaves 1..7 bytes that st3d can absorb.
bool fits(uint64_t region, uint64_t n, bool canPad) {
	return region == n || region >= n + 8 || (canPad && region > n && region < n + 8);
}

bool makePlan(const Fd& f, const Mp4FinalizeOptions& opt, const std::string* packet, Plan& p, std::string* error) {
	if (!topLevel(f, p.top, error)) return false;
	for (const Box& b : p.top.boxes)
		if (b.is("moof") || b.is("mfra") || b.is("sidx")) {
			fail(error, "fragmented MP4 not supported");
			return false;
		}
	if (!loadMoov(f, p.top, p.moovIndex, p.moov, error, &p.extraMoovs)) return false;
	const Box& moov = p.top.boxes[p.moovIndex];
	std::vector<TrackRef> tracks;
	{
		Mem mem{p.moov, moov.pos};
		if (!parseTracks(mem, moov, tracks, error)) return false;
	}
	const Box& last = p.top.boxes.back();
	if (last.toEnd) {
		if (last.size > 0xFFFFFFFFull) { fail(error, "size-0 last box over 4 GB"); return false; }
		p.normaliseLast = true;
	}
	if (packet) {
		if (packet->size() > kMaxVideoXmpPacket) {
			fail(error, "XMP packet over " + std::to_string(kMaxVideoXmpPacket) + " bytes (FORMAT.md 3.2)");
			return false;
		}
		p.xmpRequested = true;
	}
	size_t firstMdat = p.top.boxes.size();
	for (size_t i = 0; i < p.top.boxes.size(); ++i)
		if (p.top.boxes[i].is("mdat")) { firstMdat = i; break; }
	p.moovFirst = p.moovIndex < firstMdat;

	if (opt.addSt3d) {
		int added = 0;
		if (!buildSt3d(p.moov, moov.pos, tracks, 0, p.newMoov, added, error)) return false;
		p.st3dAdded = added;
		const bool grow = !p.newMoov.empty();
		const uint64_t n = grow ? p.newMoov.size() : p.moov.size();
		// Candidate regions before the first mdat: the moov itself plus the free/skip
		// boxes after it (moov first), else any run of free/skip boxes (moov at the
		// end, e.g. after an interrupted earlier run): the first that fits wins.
		for (size_t i = 0; i < firstMdat && i < p.top.boxes.size() && !p.returnToFront; ++i) {
			const Box& b = p.top.boxes[i];
			const bool startsHere = (i == p.moovIndex && grow) || (b.isFree() && !p.moovFirst);
			if (!startsHere) continue;
			uint64_t run = b.size;
			for (size_t j = i + 1; j < firstMdat; ++j) {
				const Box& c = p.top.boxes[j];
				if (!c.isFree() || c.toEnd) break;
				run += c.size;
			}
			if (run > 0xFFFFFFFFull || b.toEnd || !fits(run, n, grow)) continue;
			p.regionPos = b.pos;
			p.region = run;
			p.regionLargesize = b.largesize;
			p.returnToFront = true;
			if (grow && run > n && run < n + 8 &&
			    !buildSt3d(p.moov, moov.pos, tracks, (uint32_t)(run - n), p.newMoov, added, error))
				return false;
			if (!grow) p.newMoov = p.moov;  // relocation only
		}
		if (p.newMoov.size() > 0xFFFFFFFFull && be32(p.newMoov.data()) != 1) {
			fail(error, "moov over 4 GB");
			return false;
		}
	}
	if (packet) {
		p.xmpBox.resize(24);
		put_be32(p.xmpBox.data(), (uint32_t)(24 + packet->size()));
		std::memcpy(p.xmpBox.data() + 4, "uuid", 4);
		std::memcpy(p.xmpBox.data() + 8, kXmpUuid, 16);
		p.xmpBox.insert(p.xmpBox.end(), packet->begin(), packet->end());
	} else if (!p.newMoov.empty() && !p.returnToFront) {
		// moov is moving to the end: carry the current XMP box over so it stays after
		// moov (FORMAT.md 3.2); the old copy is retired like any replaced XMP.
		for (const Box& b : p.top.boxes)
			if (b.isXmp()) {
				if (b.size > kMaxVideoXmpPacket + 64) break;
				p.xmpBox.resize((size_t)b.size);
				if (!f.read(b.pos, p.xmpBox.data(), p.xmpBox.size())) { fail(error, "read error"); return false; }
				if (b.largesize) {  // re-emit with a 32-bit header
					std::vector<uint8_t> h(24);
					put_be32(h.data(), (uint32_t)(b.size - 8));
					std::memcpy(h.data() + 4, "uuid", 4);
					std::memcpy(h.data() + 8, kXmpUuid, 16);
					h.insert(h.end(), p.xmpBox.begin() + 32, p.xmpBox.end());
					p.xmpBox = h;
				}
				break;
			}
	}
	return true;
}

bool setType(Fd& f, uint64_t pos, const char* t) { return f.write(pos + 4, t, 4); }

// Executes a plan. Every step leaves a valid MP4 (see video.h); each step is followed
// by a sync so the order reaches the disk.
bool execute(Fd& f, Plan& p, Mp4FinalizeResult& res, std::string* error) {
	auto io = [&](bool ok) {
		if (!ok) fail(error, "write failed (file left valid; see the README crash-safety note)");
		return ok;
	};
	const Box oldMoov = p.top.boxes[p.moovIndex];
	std::vector<Box> oldXmp;  // retired only when a new XMP box replaces them
	if (!p.xmpBox.empty())
		for (const Box& b : p.top.boxes)
			if (b.isXmp()) oldXmp.push_back(b);
	const bool st3d = !p.newMoov.empty();  // a new moov is written (st3d and/or relocation)
	if (!st3d && p.xmpBox.empty()) {
		// Nothing to add; only tidy duplicate moovs an interrupted edit left behind.
		for (uint64_t pos : p.extraMoovs)
			if (!io(setType(f, pos, "free"))) return false;
		return p.extraMoovs.empty() || io(f.sync());
	}

	// 0. A size-0 last box gets its explicit size (one 4-byte write).
	if (p.normaliseLast) {
		uint8_t s[4];
		const Box& last = p.top.boxes.back();
		put_be32(s, (uint32_t)last.size);
		if (!io(f.write(last.pos, s, 4) && f.sync())) return false;
		p.top.logicalEnd = last.end();
	}

	// 1. Append at the logical end (over any < 8 trailing bytes): the new moov as a
	//    `skip` box, and the XMP box. The moov goes last when it is coming back to the
	//    front (so its staging copy can be truncated off), first otherwise (so the
	//    XMP stays after moov).
	std::vector<uint8_t> tail;
	uint64_t stagePos = 0;
	if (st3d) {
		std::vector<uint8_t> staged = p.newMoov;
		std::memcpy(staged.data() + 4, "skip", 4);
		if (p.returnToFront) {
			tail = p.xmpBox;
			stagePos = p.top.logicalEnd + tail.size();
			tail.insert(tail.end(), staged.begin(), staged.end());
		} else {
			tail = staged;
			stagePos = p.top.logicalEnd;
			tail.insert(tail.end(), p.xmpBox.begin(), p.xmpBox.end());
		}
	} else {
		tail = p.xmpBox;
	}
	const uint64_t tailEnd = p.top.logicalEnd + tail.size();
	if (!io(f.write(p.top.logicalEnd, tail.data(), tail.size()))) return false;
	if (tailEnd < p.top.fileSize && !io(f.truncate(tailEnd))) return false;  // trailing junk longer than our tail
	if (!io(f.sync())) return false;
	res.xmpWritten = p.xmpRequested;

	// 2. Hand over: the staged copy becomes moov (the old one, first in the file,
	//    still wins with readers that take the first), then the old one becomes free.
	if (st3d) {
		if (!io(setType(f, stagePos, "moov") && f.sync())) return false;
		if (!io(setType(f, oldMoov.pos, "free") && f.sync())) return false;
		res.st3dAdded = p.st3dAdded;
		res.moovAtFront = false;
		res.moovMovedToEnd = true;
	}

	// 3. Retire older XMP boxes, now that the new one is in place, and any duplicate
	//    moov an interrupted earlier edit left.
	for (const Box& b : oldXmp)
		if (!io(setType(f, b.pos, "free"))) return false;
	for (uint64_t pos : p.extraMoovs)
		if (!io(setType(f, pos, "free"))) return false;
	if ((!oldXmp.empty() || !p.extraMoovs.empty()) && !io(f.sync())) return false;

	// 4. Bring moov to the front when a region before mdat can hold it.
	if (st3d && p.returnToFront) {
		const uint64_t pos = p.regionPos, n = p.newMoov.size();
		// a. One free box over the region (old moov, now free, and/or the free boxes
		//    after it); only the size field changes, a largesize header becomes payload.
		const bool single = std::find_if(p.top.boxes.begin(), p.top.boxes.end(), [&](const Box& b) {
			                    return b.pos == pos && b.size == p.region;
		                    }) != p.top.boxes.end();
		if (!single || p.regionLargesize) {
			uint8_t s[4];
			put_be32(s, (uint32_t)p.region);
			if (!io(f.write(pos, s, 4) && f.sync())) return false;
		}
		// b. The body of the new moov, and the header of the free box after it, all
		//    inside that free box's payload.
		if (!io(f.write(pos + 8, p.newMoov.data() + 8, (size_t)(n - 8)))) return false;
		if (p.region > n) {
			uint8_t h[8];
			put_be32(h, (uint32_t)(p.region - n));
			std::memcpy(h + 4, "free", 4);
			if (!io(f.write(pos + n, h, 8))) return false;
		}
		if (!io(f.sync())) return false;
		// c. One 8-byte header write: [size n]['skip']. Torn halfway it is still valid
		//    ([free n][free rest] or [skip region]).
		uint8_t h[8];
		std::memcpy(h, p.newMoov.data(), 4);
		std::memcpy(h + 4, "skip", 4);
		if (!io(f.write(pos, h, 8) && f.sync())) return false;
		// d. It becomes moov: two identical moovs now, the front one first.
		if (!io(setType(f, pos, "moov") && f.sync())) return false;
		// e. Drop the staging copy (the last box).
		if (!io(f.truncate(stagePos) && f.sync())) return false;
		res.moovAtFront = true;
		res.moovMovedToEnd = false;
	}
	return true;
}

bool finalizeFd(int fd, const Mp4FinalizeOptions& opt, const std::string* packet, Mp4FinalizeResult* result,
                std::string* error) {
	Mp4FinalizeResult res;
	if (result) *result = res;
	if (fd < 0) { fail(error, "bad file descriptor"); return false; }
	Fd f(fd);
	Plan p;
	if (!makePlan(f, opt, packet, p, error)) return false;  // nothing written yet
	bool moovFirst = true;
	for (size_t i = 0; i < p.moovIndex; ++i)
		if (p.top.boxes[i].is("mdat")) moovFirst = false;
	res.moovAtFront = moovFirst;
	const bool ok = execute(f, p, res, error);
	if (result) *result = res;
	return ok;
}

template <class F>
bool withPath(const std::string& path, bool write, std::string* error, F&& fn) {
	const int fd = detail::open_path(path, write, error);
	if (fd < 0) return false;
	const bool ok = fn(fd);
	detail::close_fd(fd);
	return ok;
}

bool packetFor(const StereoMeta& meta, std::string& packet, std::string* error) {
	StereoMeta m = meta;
	m.layout = "sbs-lr";
	m.disparityMapIndex.reset();  // stills-only attributes (FORMAT.md 1.3, 1.4)
	m.disparityMapNear.reset();
	m.disparityMapFar.reset();
	m.disparityMapEye.clear();
	m.rawIndex.reset();
	return xmp_serialize(m, packet, error);
}

}  // namespace

// ------------------------------------------------------------ public MP4 entry points

bool mp4_finalize_fd(int fd, const Mp4FinalizeOptions& opt, Mp4FinalizeResult* result, std::string* error) {
	std::string packet;
	if (opt.xmp && !packetFor(*opt.xmp, packet, error)) {
		if (result) *result = Mp4FinalizeResult();
		return false;
	}
	return finalizeFd(fd, opt, opt.xmp ? &packet : nullptr, result, error);
}

bool mp4_finalize(const std::string& path, const Mp4FinalizeOptions& opt, Mp4FinalizeResult* result,
                  std::string* error) {
	return withPath(path, true, error, [&](int fd) { return mp4_finalize_fd(fd, opt, result, error); });
}

bool mp4_add_st3d_fd(int fd, std::string* error, int* added) {
	Mp4FinalizeOptions o;
	o.addSt3d = true;
	Mp4FinalizeResult r;
	const bool ok = finalizeFd(fd, o, nullptr, &r, error);
	if (added) *added = r.st3dAdded;
	return ok;
}

bool mp4_add_st3d(const std::string& path, std::string* error, int* added) {
	if (added) *added = 0;
	return withPath(path, true, error, [&](int fd) { return mp4_add_st3d_fd(fd, error, added); });
}

bool mp4_write_xmp_packet_fd(int fd, const std::string& packet, std::string* error) {
	Mp4FinalizeOptions o;
	o.addSt3d = false;
	return finalizeFd(fd, o, &packet, nullptr, error);
}

bool mp4_write_xmp_packet(const std::string& path, const std::string& packet, std::string* error) {
	return withPath(path, true, error, [&](int fd) { return mp4_write_xmp_packet_fd(fd, packet, error); });
}

bool mp4_write_xmp_fd(int fd, const StereoMeta& meta, std::string* error) {
	std::string packet;
	if (!packetFor(meta, packet, error)) return false;
	return mp4_write_xmp_packet_fd(fd, packet, error);
}

bool mp4_write_xmp(const std::string& path, const StereoMeta& meta, std::string* error) {
	return withPath(path, true, error, [&](int fd) { return mp4_write_xmp_fd(fd, meta, error); });
}

// ------------------------------------------------------------ probe / samples

bool mp4_probe_fd(int fd, Mp4Info& out, std::string* error) {
	out = Mp4Info();
	Fd f(fd);
	TopLevel t;
	if (!topLevel(f, t, error)) return false;
	out.trailingBytes = t.fileSize - t.logicalEnd;
	for (const Box& b : t.boxes) {
		out.topLevel.emplace_back(b.type, 4);
		if (b.isXmp() && !out.hasXmp) {
			if (b.size - b.hdr > kMaxVideoXmpPacket) { fail(error, "XMP box too large"); return false; }
			std::string p((size_t)(b.size - b.hdr), '\0');
			if (!f.read(b.body(), p.data(), p.size())) { fail(error, "read error"); return false; }
			out.hasXmp = true;
			out.xmpPacket = std::move(p);
		}
	}
	size_t mi = 0;
	std::vector<uint8_t> buf;
	if (!loadMoov(f, t, mi, buf, error)) return false;
	Mem mem{buf, t.boxes[mi].pos};
	std::vector<TrackRef> tracks;
	if (!parseTracks(mem, t.boxes[mi], tracks, error)) return false;
	for (const auto& tr : tracks) {
		if (tr.info.mime == kMetadataMime && out.metadataTrack < 0) out.metadataTrack = (int)out.tracks.size();
		out.tracks.push_back(tr.info);
	}
	return true;
}

bool mp4_probe(const std::string& path, Mp4Info& out, std::string* error) {
	out = Mp4Info();
	return withPath(path, false, error, [&](int fd) { return mp4_probe_fd(fd, out, error); });
}

bool mp4_read_samples_fd(int fd, uint32_t trackId, size_t maxSamples, std::vector<std::vector<uint8_t>>& samples,
                         std::string* error) {
	samples.clear();
	Fd f(fd);
	TopLevel t;
	if (!topLevel(f, t, error)) return false;
	size_t mi = 0;
	std::vector<uint8_t> buf;
	if (!loadMoov(f, t, mi, buf, error)) return false;
	Mem mem{buf, t.boxes[mi].pos};
	std::vector<TrackRef> tracks;
	if (!parseTracks(mem, t.boxes[mi], tracks, error)) return false;
	const TrackRef* tr = nullptr;
	for (const auto& x : tracks)
		if (x.info.trackId == trackId) tr = &x;
	if (!tr || !tr->hasStbl) { fail(error, "track not found"); return false; }
	std::vector<Box> st;
	if (!mem.children(tr->stbl.body(), tr->stbl.end(), st)) { fail(error, "broken stbl"); return false; }
	const Box* stsz = Mem::find(st, "stsz");
	const Box* stsc = Mem::find(st, "stsc");
	const Box* stco = Mem::find(st, "stco");
	const Box* co64 = Mem::find(st, "co64");
	if (!stsz || !stsc || (!stco && !co64)) { fail(error, "missing sample tables"); return false; }

	auto body = [&](const Box* b, uint64_t& n) { n = b->size - b->hdr; return mem.at(b->body()); };
	uint64_t n = 0;
	const uint8_t* z = body(stsz, n);
	if (n < 12) { fail(error, "broken stsz"); return false; }
	const uint32_t fixed = be32(z + 4), count = be32(z + 8);
	if (fixed == 0 && (uint64_t)count > (n - 12) / 4) { fail(error, "broken stsz"); return false; }
	const uint8_t* c = body(stsc, n);
	if (n < 8) { fail(error, "broken stsc"); return false; }
	const uint32_t runs = be32(c + 4);
	if ((uint64_t)runs > (n - 8) / 12) { fail(error, "broken stsc"); return false; }
	// Runs: first_chunk starts at 1 and strictly increases; samples_per_chunk >= 1.
	for (uint32_t r = 0; r < runs; ++r) {
		const uint32_t first = be32(c + 8 + r * 12), spc = be32(c + 8 + r * 12 + 4);
		if (spc == 0 || (r == 0 && first != 1) || (r > 0 && first <= be32(c + 8 + (r - 1) * 12))) {
			fail(error, "malformed stsc");
			return false;
		}
	}
	const Box* cb = co64 ? co64 : stco;
	const uint8_t* o = body(cb, n);
	if (n < 8) { fail(error, "broken chunk offsets"); return false; }
	const uint32_t chunks = be32(o + 4);
	const uint64_t w = co64 ? 8 : 4;
	if ((uint64_t)chunks > (n - 8) / w) { fail(error, "broken chunk offsets"); return false; }

	// One pass: each chunk yields >= 1 sample, so this is O(min(chunks, count) + runs).
	uint32_t sample = 0, run = 0;
	for (uint32_t ci = 0; ci < chunks && sample < count && samples.size() < maxSamples; ++ci) {
		if (runs == 0) break;
		while (run + 1 < runs && be32(c + 8 + (run + 1) * 12) <= ci + 1) ++run;
		const uint32_t spc = be32(c + 8 + run * 12 + 4);
		uint64_t off = co64 ? be64(o + 8 + (uint64_t)ci * 8) : be32(o + 8 + (uint64_t)ci * 4);
		for (uint32_t k = 0; k < spc && sample < count && samples.size() < maxSamples; ++k, ++sample) {
			const uint32_t sz = fixed ? fixed : be32(z + 12 + (uint64_t)sample * 4);
			if (off > t.fileSize || sz > t.fileSize - off || sz > (64u << 20)) {
				fail(error, "sample outside the file");
				return false;
			}
			std::vector<uint8_t> s(sz);
			if (sz && !f.read(off, s.data(), sz)) { fail(error, "read error"); return false; }
			samples.push_back(std::move(s));
			off += sz;
		}
	}
	return true;
}

bool mp4_read_samples(const std::string& path, uint32_t trackId, size_t maxSamples,
                      std::vector<std::vector<uint8_t>>& samples, std::string* error) {
	samples.clear();
	return withPath(path, false, error,
	                [&](int fd) { return mp4_read_samples_fd(fd, trackId, maxSamples, samples, error); });
}

}  // namespace dxrmedia
