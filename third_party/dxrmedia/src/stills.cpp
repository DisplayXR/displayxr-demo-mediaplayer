// SPDX-License-Identifier: Apache-2.0
#include "dxrmedia/stills.h"

#include "dxrmedia/xmp.h"
#include "exif.h"
#include "tiff.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace dxrmedia {

namespace {

const char kXmpSig[] = "http://ns.adobe.com/xap/1.0/";            // + NUL = 29 bytes
const char kXmpExtSig[] = "http://ns.adobe.com/xmp/extension/";   // + NUL
const uint8_t kExifSig[6] = {'E', 'x', 'i', 'f', 0, 0};
const uint8_t kMpfSig[4] = {'M', 'P', 'F', 0};

constexpr size_t kMaxSegPayload = 65533;  // segment length field covers itself

// MPF tags (CIPA DC-007).
enum : uint16_t {
	kMpfVersion = 0xB000,
	kNumberOfImages = 0xB001,
	kMpEntry = 0xB002,
	kMpIndividualNum = 0xB101,
	kBaseViewpointNum = 0xB204,
	kConvergenceAngle = 0xB205,
	kBaselineLength = 0xB206,
};

constexpr uint32_t kAttrPrimaryDisparity = 0x20020002u;  // representative | multi-view disparity | JPEG
constexpr uint32_t kAttrDisparity = 0x00020002u;
constexpr uint32_t kAttrUndefined = 0x00000000u;

void fail(std::string* e, const std::string& m) {
	if (e) *e = m;
}

struct Segment {
	uint8_t marker = 0;
	size_t pos = 0;      // offset of the 0xFF
	size_t payload = 0;  // offset of the payload (after the length)
	size_t end = 0;      // offset just after the segment
};

// Walk the marker chain from just after SOI up to (not including) SOS.
// `sos` = offset of the SOS marker. False if the header is truncated or malformed.
bool scanHeader(const uint8_t* b, size_t n, std::vector<Segment>& segs, size_t& sos,
                std::string& err, int* resyncs = nullptr) {
	segs.clear();
	if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) { err = "not a JPEG (no SOI)"; return false; }
	size_t pos = 2;
	while (pos + 2 <= n) {
		if (b[pos] != 0xFF) {
			if (resyncs) ++*resyncs;
			++pos;
			continue;
		}
		const uint8_t m = b[pos + 1];
		if (m == 0xFF) { ++pos; continue; }
		if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { pos += 2; continue; }
		if (m == 0xD8 || m == 0xD9) { err = "unexpected SOI/EOI in JPEG header"; return false; }
		if (m == 0xDA) { sos = pos; return true; }
		if (pos + 4 > n) break;
		const size_t len = ((size_t)b[pos + 2] << 8) | b[pos + 3];
		if (len < 2) { err = "JPEG segment with bad length"; return false; }
		if (pos + 2 + len > n) break;
		segs.push_back(Segment{m, pos, pos + 4, pos + 2 + len});
		pos += 2 + len;
	}
	err = "JPEG header truncated (no SOS)";
	return false;
}

// The real EOI of a JPEG, walking markers from the first SOS at `sos`: marker segments
// (DHT, DQT, DRI, COM, APPn, SOFn, SOS headers between progressive scans) are skipped
// by their length, so an FF D9 inside their payload is not mistaken for EOI; inside
// entropy-coded data FF 00 (stuffing), FF D0-D7 (RSTn) and FF FF (fill) are data.
// `eoiEnd` = offset just after the EOI marker.
bool findEoi(const uint8_t* b, size_t n, size_t sos, size_t& eoiEnd) {
	size_t pos = sos;
	for (;;) {
		// At a marker position.
		if (pos + 2 > n || b[pos] != 0xFF) return false;
		while (pos + 1 < n && b[pos + 1] == 0xFF) ++pos;  // fill bytes before a marker
		if (pos + 2 > n) return false;
		const uint8_t m = b[pos + 1];
		if (m == 0xD9) { eoiEnd = pos + 2; return true; }
		if (m == 0x00 || m == 0xD8) return false;
		if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) { pos += 2; continue; }
		if (pos + 4 > n) return false;
		const size_t len = ((size_t)b[pos + 2] << 8) | b[pos + 3];
		if (len < 2 || pos + 2 + len > n) return false;
		pos += 2 + len;
		if (m != 0xDA) continue;  // a non-scan segment: next thing is another marker
		// Entropy-coded data until a marker that is not stuffing / RSTn / fill.
		for (;;) {
			if (pos >= n) return false;
			if (b[pos] != 0xFF) { ++pos; continue; }
			if (pos + 1 >= n) return false;
			const uint8_t d = b[pos + 1];
			if (d == 0x00 || (d >= 0xD0 && d <= 0xD7)) { pos += 2; continue; }
			if (d == 0xFF) { ++pos; continue; }
			break;  // a real marker at pos
		}
	}
}

bool payloadStarts(const uint8_t* b, const Segment& s, const void* sig, size_t len) {
	return s.end - s.payload >= len && std::memcmp(b + s.payload, sig, len) == 0;
}

bool isStripped(const uint8_t* b, const Segment& s) {
	if (s.marker == 0xE0)
		return payloadStarts(b, s, "JFIF\0", 5) || payloadStarts(b, s, "JFXX\0", 5);
	if (s.marker == 0xE1)
		return payloadStarts(b, s, kExifSig, 6) || payloadStarts(b, s, kXmpSig, sizeof(kXmpSig)) ||
		       payloadStarts(b, s, kXmpExtSig, sizeof(kXmpExtSig));
	if (s.marker == 0xE2) return payloadStarts(b, s, kMpfSig, 4);
	return false;
}

// The input JPEG without SOI and without the segments the writer replaces; ends at EOI.
bool stripInput(Bytes in, const char* what, std::vector<uint8_t>& rest, std::string* error,
                StillWriteReport* report) {
	std::vector<Segment> segs;
	size_t sos = 0;
	std::string err;
	if (!in.data || !scanHeader(in.data, in.size, segs, sos, err)) {
		fail(error, std::string(what) + ": " + (in.data ? err : "missing"));
		return false;
	}
	size_t eoiEnd = 0;
	if (!findEoi(in.data, in.size, sos, eoiEnd)) {
		fail(error, std::string(what) + ": no EOI (truncated JPEG)");
		return false;
	}
	rest.clear();
	for (const Segment& s : segs) {
		if (!isStripped(in.data, s)) {
			rest.insert(rest.end(), in.data + s.pos, in.data + s.end);
		} else if (s.marker == 0xE2 && report) {
			report->droppedEncoderMpf = true;
			report->warnings.push_back(std::string(what) + ": the encoder's own MPF index was dropped");
		}
	}
	rest.insert(rest.end(), in.data + sos, in.data + eoiEnd);
	// Bytes after EOI (an Ultra HDR gain map, another MPF image, ...) are not kept.
	bool trailing = false;
	for (size_t i = eoiEnd; i < in.size && !trailing; ++i) trailing = in.data[i] != 0;
	if (trailing && report) {
		report->droppedTrailingData = true;
		report->warnings.push_back(std::string(what) + ": " + std::to_string(in.size - eoiEnd) +
		                           " bytes after EOI were dropped (e.g. an Ultra HDR gain map)");
	}
	return true;
}

bool appendSegment(std::vector<uint8_t>& o, uint8_t marker, const std::vector<uint8_t>& payload) {
	if (payload.size() > kMaxSegPayload) return false;
	const size_t len = payload.size() + 2;
	o.push_back(0xFF);
	o.push_back(marker);
	o.push_back((uint8_t)(len >> 8));
	o.push_back((uint8_t)len);
	o.insert(o.end(), payload.begin(), payload.end());
	return true;
}

void attributeIfd(tiff::Builder& b, uint32_t individual, bool disparity,
                  const std::optional<double>& baselineMm) {
	b.undefined(kMpfVersion, {'0', '1', '0', '0'});
	b.u32(kMpIndividualNum, individual);
	if (disparity) {
		b.u32(kBaseViewpointNum, 1);
		b.srational(kConvergenceAngle, 0, 1);  // parallel rig
		if (baselineMm && *baselineMm > 0 && std::isfinite(*baselineMm)) {
			uint32_t n = 0, d = 1;
			tiff::to_rational(*baselineMm / 1000.0, n, d);  // MPF BaselineLength is metres
			b.rational(kBaselineLength, n, d);
		}
	}
}

struct MpEntryW {
	uint32_t attr, size, offset;
};

// MPF APP2 payload for image `idx` (0-based). Image 0 carries the index IFD whose
// next-IFD pointer is its own attribute IFD; the others an attribute IFD only.
std::vector<uint8_t> mpfPayload(size_t idx, const std::vector<MpEntryW>& entries,
                                const std::optional<double>& baselineMm) {
	const bool be = true;
	std::vector<uint8_t> o(kMpfSig, kMpfSig + 4);
	std::vector<uint8_t> t = {'M', 'M', 0, 42, 0, 0, 0, 8};
	const bool disparity = idx < 2;
	tiff::Builder attr;
	attr.be = be;
	attributeIfd(attr, (uint32_t)idx + 1, disparity, baselineMm);
	if (idx == 0) {
		tiff::Builder index;
		index.be = be;
		index.undefined(kMpfVersion, {'0', '1', '0', '0'});
		index.u32(kNumberOfImages, (uint32_t)entries.size());
		std::vector<uint8_t> e;
		for (const MpEntryW& m : entries) {
			tiff::put32(e, m.attr, be);
			tiff::put32(e, m.size, be);
			tiff::put32(e, m.offset, be);
			tiff::put16(e, 0, be);
			tiff::put16(e, 0, be);
		}
		index.undefined(kMpEntry, e);
		const uint32_t attrOff = 8 + (uint32_t)index.size();
		index.write(t, 8, attrOff);
		attr.write(t, attrOff, 0);
	} else {
		attr.write(t, 8, 0);
	}
	o.insert(o.end(), t.begin(), t.end());
	return o;
}

// Frame size from the first SOFn segment.
bool sofSize(Bytes in, int& w, int& h) {
	std::vector<Segment> segs;
	size_t sos = 0;
	std::string err;
	if (!in.data || !scanHeader(in.data, in.size, segs, sos, err)) return false;
	for (const Segment& s : segs) {
		const uint8_t m = s.marker;
		const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
		if (!sof || s.end - s.payload < 5) continue;
		const uint8_t* p = in.data + s.payload;
		h = (p[1] << 8) | p[2];
		w = (p[3] << 8) | p[4];
		return w > 0 && h > 0;
	}
	return false;
}

// FORMAT.md 1.2: EXIF PixelX/YDimension = the STORED image (always set from its frame
// header, overriding the caller). FORMAT.md 1.3: dxr:EyeWidth/EyeHeight = one eye
// (filled only when the caller left them out).
void fillSizes(int storedW, int storedH, int eyeW, int eyeH, ExifMeta& exif, StereoMeta& stereo) {
	exif.pixelXDimension = (uint32_t)storedW;
	exif.pixelYDimension = (uint32_t)storedH;
	if (!stereo.eyeWidth) stereo.eyeWidth = eyeW;
	if (!stereo.eyeHeight) stereo.eyeHeight = eyeH;
}

bool buildHeaderSegments(const ExifMeta& exifMeta, const StereoMeta& stereo, std::vector<uint8_t>& o,
                         std::string* error) {
	std::vector<uint8_t> exif(kExifSig, kExifSig + 6);
	const std::vector<uint8_t> tiffBlock = exif_build(exifMeta, stereo.facing);
	exif.insert(exif.end(), tiffBlock.begin(), tiffBlock.end());
	if (!appendSegment(o, 0xE1, exif)) { fail(error, "EXIF block over 64 KB"); return false; }

	std::string packet;
	std::string xerr;
	if (!xmp_serialize(stereo, packet, &xerr)) {
		fail(error, "XMP: " + xerr);
		return false;
	}
	std::vector<uint8_t> xmp(kXmpSig, kXmpSig + sizeof(kXmpSig));  // includes the NUL
	xmp.insert(xmp.end(), packet.begin(), packet.end());
	if (!appendSegment(o, 0xE1, xmp)) {
		fail(error, "XMP packet over 64 KB (FORMAT.md 1.3 forbids extended XMP)");
		return false;
	}
	return true;
}

}  // namespace

bool write_still(const StillInputs& in, const StillMeta& meta, std::vector<uint8_t>& out,
                 std::string* error, StillWriteReport* report) {
	out.clear();
	if (report) *report = StillWriteReport();
	if (in.left.empty() || in.right.empty()) { fail(error, "left and right views are required"); return false; }
	if (in.rawLeft.empty() != in.rawRight.empty()) {
		fail(error, "raw left and raw right come together");
		return false;
	}
	std::vector<Bytes> inputs = {in.left, in.right};
	const char* names[] = {"left", "right", "disparity map", "raw left", "raw right"};
	std::vector<const char*> inNames = {names[0], names[1]};
	StereoMeta stereo = meta.stereo;
	stereo.version = kFormatVersion;
	stereo.layout = "mpf-pair";
	stereo.disparityMapIndex.reset();
	stereo.rawIndex.reset();
	if (!in.disparityMap.empty()) {
		stereo.disparityMapIndex = (int)inputs.size();
		if (stereo.disparityMapEye.empty()) stereo.disparityMapEye = "left";
		inputs.push_back(in.disparityMap);
		inNames.push_back(names[2]);
	}
	if (!in.rawLeft.empty()) {
		stereo.rawIndex = (int)inputs.size();
		inputs.push_back(in.rawLeft);
		inputs.push_back(in.rawRight);
		inNames.push_back(names[3]);
		inNames.push_back(names[4]);
	}

	ExifMeta exifMeta = meta.exif;
	int lw = 0, lh = 0, rw = 0, rh = 0;
	if (sofSize(in.left, lw, lh) && sofSize(in.right, rw, rh)) {
		if (lw != rw || lh != rh) {
			fail(error, "left and right views differ in size (" + std::to_string(lw) + "x" + std::to_string(lh) +
			                " vs " + std::to_string(rw) + "x" + std::to_string(rh) + ")");
			return false;
		}
		fillSizes(lw, lh, lw, lh, exifMeta, stereo);
	}

	std::vector<std::vector<uint8_t>> rest(inputs.size());
	for (size_t i = 0; i < inputs.size(); ++i)
		if (!stripInput(inputs[i], inNames[i], rest[i], error, report)) return false;

	const size_t n = inputs.size();
	std::vector<MpEntryW> entries(n, MpEntryW{0, 0, 0});

	// Image 0 header: SOI, Exif, XMP, then MPF (its size does not depend on the values).
	std::vector<uint8_t> head0 = {0xFF, 0xD8};
	if (!buildHeaderSegments(exifMeta, stereo, head0, error)) return false;
	const size_t mpfMarkerAt = head0.size();
	const uint64_t tiffBase = mpfMarkerAt + 4 + 4;  // FF E2 len len "MPF\0"
	std::vector<std::vector<uint8_t>> mpf(n);
	for (size_t i = 0; i < n; ++i) mpf[i] = mpfPayload(i, entries, stereo.baselineMm);

	std::vector<uint64_t> size(n), offset(n);
	uint64_t at = 0;
	for (size_t i = 0; i < n; ++i) {
		const uint64_t hdr = i == 0 ? head0.size() : 2;
		size[i] = hdr + 4 + mpf[i].size() + rest[i].size();
		offset[i] = at;
		at += size[i];
	}
	if (at > 0xFFFFFFFFull) { fail(error, "file over 4 GB (MPF offsets are 32-bit)"); return false; }
	for (size_t i = 0; i < n; ++i) {
		entries[i].attr = i == 0 ? kAttrPrimaryDisparity : (i == 1 ? kAttrDisparity : kAttrUndefined);
		entries[i].size = (uint32_t)size[i];
		entries[i].offset = i == 0 ? 0 : (uint32_t)(offset[i] - tiffBase);
	}
	mpf[0] = mpfPayload(0, entries, stereo.baselineMm);

	out.reserve((size_t)at);
	for (size_t i = 0; i < n; ++i) {
		if (i == 0) out.insert(out.end(), head0.begin(), head0.end());
		else { out.push_back(0xFF); out.push_back(0xD8); }
		if (!appendSegment(out, 0xE2, mpf[i])) { fail(error, "MPF segment over 64 KB"); return false; }
		out.insert(out.end(), rest[i].begin(), rest[i].end());
	}
	if (out.size() != at) { fail(error, "internal: layout size mismatch"); out.clear(); return false; }
	return true;
}

bool write_sbs_still(Bytes sbs, const StillMeta& meta, std::vector<uint8_t>& out,
                     std::string* error, StillWriteReport* report) {
	out.clear();
	if (report) *report = StillWriteReport();
	std::vector<uint8_t> rest;
	if (!stripInput(sbs, "side-by-side image", rest, error, report)) return false;
	StereoMeta stereo = meta.stereo;
	stereo.version = kFormatVersion;
	stereo.layout = "sbs-lr";
	stereo.disparityMapIndex.reset();
	stereo.rawIndex.reset();
	stereo.disparityMapEye.clear();
	stereo.disparityMapNear.reset();
	stereo.disparityMapFar.reset();
	ExifMeta exifMeta = meta.exif;
	int w = 0, h = 0;
	if (sofSize(sbs, w, h)) fillSizes(w, h, w / 2, h, exifMeta, stereo);
	out = {0xFF, 0xD8};
	if (!buildHeaderSegments(exifMeta, stereo, out, error)) { out.clear(); return false; }
	out.insert(out.end(), rest.begin(), rest.end());
	return true;
}

// ------------------------------------------------------------------- reader

const char* to_string(ReadStatus s) {
	switch (s) {
	case ReadStatus::Ours: return "ours";
	case ReadStatus::NotOurs: return "not-ours";
	case ReadStatus::Invalid: return "invalid";
	}
	return "?";
}

const ImageSpan* StillFile::right() const {
	return hasMpf && images.size() > 1 ? &images[1] : nullptr;
}

const ImageSpan* StillFile::disparityMap() const {
	const auto& i = meta.stereo.disparityMapIndex;
	return hasMpf && i && *i > 0 && (size_t)*i < images.size() ? &images[(size_t)*i] : nullptr;
}

const ImageSpan* StillFile::rawLeft() const {
	const auto& i = meta.stereo.rawIndex;
	return hasMpf && i && *i > 0 && (size_t)*i < images.size() ? &images[(size_t)*i] : nullptr;
}

const ImageSpan* StillFile::rawRight() const {
	const auto& i = meta.stereo.rawIndex;
	return hasMpf && i && *i > 0 && (size_t)*i + 1 < images.size() ? &images[(size_t)*i + 1] : nullptr;
}

namespace {

StillFile invalid(StillFile f, const std::string& why) {
	f.status = ReadStatus::Invalid;
	f.error = why;
	f.images.clear();
	return f;
}

// A span must start with SOI and end with EOI (a few bytes of padding tolerated).
bool spanLooksLikeJpeg(const uint8_t* b, uint64_t off, uint64_t size) {
	if (size < 4) return false;
	if (b[off] != 0xFF || b[off + 1] != 0xD8) return false;
	const uint64_t end = off + size;
	for (uint64_t k = 0; k < 16 && end - k >= off + 4; ++k)
		if (b[end - k - 2] == 0xFF && b[end - k - 1] == 0xD9) return true;
	return false;
}

}  // namespace

StillFile read_still(Bytes file) {
	StillFile f;
	const uint8_t* b = file.data;
	const size_t n = file.size;
	if (!b || n == 0) return invalid(f, "empty input");

	std::vector<Segment> segs;
	size_t sos = 0;
	std::string err;
	int resyncs = 0;
	if (!scanHeader(b, n, segs, sos, err, &resyncs)) return invalid(f, err);
	if (resyncs) f.warnings.push_back("stray bytes between JPEG segments");

	size_t mpfBase = 0;
	for (const Segment& s : segs) {
		if (s.marker == 0xE1 && !f.hasExif && payloadStarts(b, s, kExifSig, 6)) {
			f.hasExif = exif_parse(b + s.payload + 6, s.end - s.payload - 6, f.meta.exif);
			if (!f.hasExif) f.warnings.push_back("EXIF block present but unreadable");
		} else if (s.marker == 0xE1 && !f.hasXmp && payloadStarts(b, s, kXmpSig, sizeof(kXmpSig))) {
			f.hasXmp = true;
			f.xmpPacket.assign(reinterpret_cast<const char*>(b + s.payload + sizeof(kXmpSig)),
			                   s.end - s.payload - sizeof(kXmpSig));
		} else if (s.marker == 0xE2 && !f.hasMpf && payloadStarts(b, s, kMpfSig, 4)) {
			f.hasMpf = true;
			mpfBase = s.payload + 4;
		}
	}
	const bool ours = f.hasXmp && xmp_parse(f.xmpPacket, f.meta.stereo);

	if (!f.hasMpf) {
		size_t eoiEnd = 0;
		if (!findEoi(b, n, sos, eoiEnd)) return invalid(f, "no EOI (truncated JPEG)");
		f.images.push_back(ImageSpan{0, eoiEnd, 0});
	} else {
		// The MPF segment's TIFF block is bounded by the file, not by the segment:
		// offsets point past it. IFDs themselves must lie inside the segment though.
		size_t segEnd = 0;
		for (const Segment& s : segs)
			if (s.payload + 4 == mpfBase) segEnd = s.end;
		tiff::Reader r;
		r.b = b + mpfBase;
		r.n = segEnd - mpfBase;
		uint32_t first = 0;
		if (!r.header(first)) return invalid(f, "MPF: bad TIFF header");
		std::vector<tiff::Reader::Field> fields;
		uint32_t next = 0;
		if (!r.ifd(first, fields, next)) return invalid(f, "MPF: index IFD out of bounds");
		const auto* ver = tiff::Reader::find(fields, kMpfVersion);
		if (!ver || r.asString(*ver).compare(0, 4, "0100") != 0)
			f.warnings.push_back("MPF: MPFVersion missing or not 0100");
		const auto* num = tiff::Reader::find(fields, kNumberOfImages);
		const auto* ent = tiff::Reader::find(fields, kMpEntry);
		uint32_t count = 0;
		if (!num || !r.asU32(*num, count) || !ent) return invalid(f, "MPF: no NumberOfImages / MPEntry");
		if (ent->valueSize / 16 != count) {
			f.warnings.push_back("MPF: NumberOfImages disagrees with MPEntry size");
			if (ent->valueSize / 16 < count) count = (uint32_t)(ent->valueSize / 16);
		}
		if (count < 1) return invalid(f, "MPF: no images");
		for (uint32_t i = 0; i < count; ++i) {
			const size_t e = ent->valueAt + (size_t)i * 16;
			uint32_t attr = 0, size = 0, off = 0;
			if (!r.u32(e, attr) || !r.u32(e + 4, size) || !r.u32(e + 8, off))
				return invalid(f, "MPF: MPEntry out of bounds");
			const uint64_t fileOff = i == 0 ? 0 : (uint64_t)mpfBase + off;
			if (i == 0 && off != 0) f.warnings.push_back("MPF: first entry offset is not 0");
			if (i > 0 && off == 0) return invalid(f, "MPF: entry " + std::to_string(i) + " has offset 0");
			if (fileOff >= n || size > n - fileOff)
				return invalid(f, "MPF: entry " + std::to_string(i) + " lies outside the file (truncated?)");
			if (!spanLooksLikeJpeg(b, fileOff, size))
				return invalid(f, "MPF: entry " + std::to_string(i) + " does not span SOI..EOI");
			f.images.push_back(ImageSpan{fileOff, size, attr});
		}
	}

	if (ours) {
		f.status = ReadStatus::Ours;
		const StereoMeta& m = f.meta.stereo;
		if (m.layout == "mpf-pair" && f.images.size() < 2)
			f.warnings.push_back("dxr:Layout is mpf-pair but the file has no second image");
		if (m.disparityMapIndex && !f.disparityMap())
			f.warnings.push_back("dxr:DisparityMapIndex points at no image");
		if (m.rawIndex && !f.rawRight()) f.warnings.push_back("dxr:RawIndex points at no image pair");
	} else {
		f.status = ReadStatus::NotOurs;
	}
	return f;
}

StillFile read_still_file(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return invalid(StillFile(), "cannot open " + path);
	std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return read_still(Bytes(buf));
}

}  // namespace dxrmedia
