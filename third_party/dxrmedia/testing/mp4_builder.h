// SPDX-License-Identifier: Apache-2.0
// Test helper: builds small MP4 files in the layouts the post-pass must handle.
#pragma once

#include "dxrmedia/video.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace mp4b {
using namespace dxrmedia;

inline void b32(std::vector<uint8_t>& o, uint32_t v) {
	o.push_back((uint8_t)(v >> 24)); o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v);
}
inline void b64(std::vector<uint8_t>& o, uint64_t v) { b32(o, (uint32_t)(v >> 32)); b32(o, (uint32_t)v); }
inline void b16(std::vector<uint8_t>& o, uint16_t v) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
inline std::vector<uint8_t> box(const char* t, const std::vector<uint8_t>& payload) {
	std::vector<uint8_t> o;
	b32(o, (uint32_t)(8 + payload.size()));
	o.insert(o.end(), t, t + 4);
	o.insert(o.end(), payload.begin(), payload.end());
	return o;
}
inline std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> v) {
	std::vector<uint8_t> o;
	for (const auto& x : v) o.insert(o.end(), x.begin(), x.end());
	return o;
}
inline std::vector<uint8_t> zeros(size_t n) { return std::vector<uint8_t>(n, 0); }

struct TrackSpec {
	std::string handler;              // "vide" or "meta"
	std::vector<std::vector<uint8_t>> samples;
	bool terminator = false;          // vide: 4 zero bytes at the end of the sample entry
};

struct LayoutSpec {
	bool moovFirst = true;
	int freeAfterMoov = -1;           // bytes of a free box right after moov (-1: none)
	bool co64 = false;
	bool size0Last = false;           // the last box (moov or mdat) has size 0
	int trailing = 0;                 // junk bytes after the last box (< 8)
};

inline std::vector<uint8_t> sample_entry(const TrackSpec& t) {
	if (t.handler == "vide") {
		std::vector<uint8_t> e = zeros(6);
		b16(e, 1);                                   // data_reference_index
		auto v = zeros(16); e.insert(e.end(), v.begin(), v.end());
		b16(e, 640); b16(e, 180);                    // width, height
		b32(e, 0x00480000); b32(e, 0x00480000); b32(e, 0); b16(e, 1);
		auto name = zeros(32); e.insert(e.end(), name.begin(), name.end());
		b16(e, 0x0018); b16(e, 0xFFFF);
		const auto hvcc = box("hvcC", {1, 2, 3, 4, 5, 6, 7, 8});
		e.insert(e.end(), hvcc.begin(), hvcc.end());
		if (t.terminator) { auto z = zeros(4); e.insert(e.end(), z.begin(), z.end()); }
		return box("hvc1", e);
	}
	std::vector<uint8_t> mett = zeros(6);
	b16(mett, 1);
	mett.push_back(0);
	const std::string mime = kMetadataMime;
	mett.insert(mett.end(), mime.begin(), mime.end());
	mett.push_back(0);
	return box("mett", mett);
}

inline std::vector<uint8_t> trak(const TrackSpec& t, uint32_t id, uint64_t dataOff, bool co64) {
	const uint32_t mat[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
	std::vector<uint8_t> tkhd = {0, 0, 0, 3};
	b32(tkhd, 0); b32(tkhd, 0); b32(tkhd, id); b32(tkhd, 0); b32(tkhd, (uint32_t)t.samples.size());
	auto z = zeros(16); tkhd.insert(tkhd.end(), z.begin(), z.end());
	for (uint32_t x : mat) b32(tkhd, x);
	b32(tkhd, t.handler == "vide" ? 640u << 16 : 0); b32(tkhd, t.handler == "vide" ? 180u << 16 : 0);
	std::vector<uint8_t> mdhd = zeros(12);
	b32(mdhd, 30); b32(mdhd, (uint32_t)t.samples.size()); b16(mdhd, 0x55C4); b16(mdhd, 0);
	std::vector<uint8_t> hdlr = zeros(8);
	hdlr.insert(hdlr.end(), t.handler.begin(), t.handler.end());
	auto hz = zeros(13); hdlr.insert(hdlr.end(), hz.begin(), hz.end());
	std::vector<uint8_t> stsd = zeros(4);
	b32(stsd, 1);
	const auto se = sample_entry(t);
	stsd.insert(stsd.end(), se.begin(), se.end());
	std::vector<uint8_t> stts = zeros(4); b32(stts, 1); b32(stts, (uint32_t)t.samples.size()); b32(stts, 1);
	// Two samples per chunk; an odd tail gets its own stsc run (as muxers write it).
	const uint32_t nChunks = (uint32_t)((t.samples.size() + 1) / 2);
	const bool odd = t.samples.size() % 2 == 1 && t.samples.size() > 1;
	std::vector<uint8_t> stsc = zeros(4);
	b32(stsc, odd ? 2 : 1);
	b32(stsc, 1); b32(stsc, t.samples.size() == 1 ? 1 : 2); b32(stsc, 1);
	if (odd) { b32(stsc, nChunks); b32(stsc, 1); b32(stsc, 1); }
	std::vector<uint8_t> stsz = zeros(4); b32(stsz, 0); b32(stsz, (uint32_t)t.samples.size());
	for (const auto& s : t.samples) b32(stsz, (uint32_t)s.size());
	std::vector<uint8_t> co = zeros(4);
	b32(co, nChunks);
	uint64_t off = dataOff;
	for (size_t i = 0; i < t.samples.size(); ++i) {
		if (i % 2 == 0) { if (co64) b64(co, off); else b32(co, (uint32_t)off); }
		off += t.samples[i].size();
	}
	std::vector<uint8_t> url = {0, 0, 0, 1};
	std::vector<uint8_t> dref = zeros(4); b32(dref, 1);
	auto u = box("url ", url); dref.insert(dref.end(), u.begin(), u.end());
	auto stbl = box("stbl", cat({box("stsd", stsd), box("stts", stts), box("stsc", stsc), box("stsz", stsz),
	                             box(co64 ? "co64" : "stco", co)}));
	auto minf = box("minf", cat({box(t.handler == "vide" ? "vmhd" : "nmhd", zeros(t.handler == "vide" ? 12 : 4)),
	                             box("dinf", box("dref", dref)), stbl}));
	auto mdia = box("mdia", cat({box("mdhd", mdhd), box("hdlr", hdlr), minf}));
	return box("trak", cat({box("tkhd", tkhd), mdia}));
}

inline std::vector<uint8_t> build_mp4(const std::vector<TrackSpec>& tracks, const LayoutSpec& L) {
	const std::vector<uint8_t> ftyp = box("ftyp", {'i', 's', 'o', 'm', 0, 0, 2, 0, 'i', 's', 'o', 'm', 'm', 'p', '4', '1'});
	std::vector<uint8_t> data;
	for (const auto& t : tracks)
		for (const auto& s : t.samples) data.insert(data.end(), s.begin(), s.end());
	auto moovAt = [&](uint64_t mdatBody) {
		std::vector<uint8_t> mvhd = zeros(12);
		b32(mvhd, 1000); b32(mvhd, 100); b32(mvhd, 0x00010000); b16(mvhd, 0x0100);
		auto r = zeros(10); mvhd.insert(mvhd.end(), r.begin(), r.end());
		const uint32_t mat[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
		for (uint32_t x : mat) b32(mvhd, x);
		auto pd = zeros(24); mvhd.insert(mvhd.end(), pd.begin(), pd.end());
		b32(mvhd, (uint32_t)tracks.size() + 1);
		std::vector<uint8_t> body = box("mvhd", mvhd);
		uint64_t off = mdatBody;
		for (size_t i = 0; i < tracks.size(); ++i) {
			const auto tk = trak(tracks[i], (uint32_t)i + 1, off, L.co64);
			body.insert(body.end(), tk.begin(), tk.end());
			for (const auto& s : tracks[i].samples) off += s.size();
		}
		return box("moov", body);
	};
	const std::vector<uint8_t> freeBox = L.freeAfterMoov >= 8 ? box("free", zeros((size_t)L.freeAfterMoov - 8))
	                                                          : std::vector<uint8_t>();
	const size_t moovSize = moovAt(0).size();
	std::vector<uint8_t> mdat = box("mdat", data);
	std::vector<uint8_t> out;
	if (L.moovFirst) {
		const uint64_t body = ftyp.size() + moovSize + freeBox.size() + 8;
		out = cat({ftyp, moovAt(body), freeBox, mdat});
		if (L.size0Last) { const size_t p = ftyp.size() + moovSize + freeBox.size(); out[p] = out[p + 1] = out[p + 2] = out[p + 3] = 0; }
	} else {
		const uint64_t body = ftyp.size() + 8;
		out = cat({ftyp, mdat, moovAt(body)});
		if (L.size0Last) { const size_t p = ftyp.size() + mdat.size(); out[p] = out[p + 1] = out[p + 2] = out[p + 3] = 0; }
	}
	for (int i = 0; i < L.trailing; ++i) out.push_back((uint8_t)(0xA0 + i));
	return out;
}

inline std::vector<TrackSpec> two_tracks(int videoFrames = 5, bool terminator = false) {
	TrackSpec v;
	v.handler = "vide";
	v.terminator = terminator;
	for (int i = 0; i < videoFrames; ++i) v.samples.push_back(std::vector<uint8_t>(100 + 37 * (size_t)i, (uint8_t)(0x10 + i)));
	TrackSpec m;
	m.handler = "meta";
	for (int i = 0; i < videoFrames; ++i) {
		FrameMeta f;
		f.convergenceValid = true;
		f.convergence = 0.01f * (float)i;
		f.exposureValid = true;
		f.iso = (uint16_t)(100 + i);
		const auto e = encode_frame_meta(f);
		m.samples.emplace_back(e.begin(), e.end());
	}
	return {v, m};
}

}  // namespace mp4b
