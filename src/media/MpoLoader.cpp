// SPDX-License-Identifier: Apache-2.0
#include "MpoLoader.h"

#include "Log.h"
#include "StereoCompose.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace mp {

namespace {

// How much of the header IsMpo() reads. An APP segment caps at 65533 bytes, and a JPEG
// may carry a few (APP0 JFIF, APP1 Exif with a thumbnail, APP1 XMP) before APP2, so this
// covers any realistic layout. Load() reads the whole file and has no such bound.
constexpr size_t kSniffBytes = 256 * 1024;

// MP Type codes (attribute & 0x00FFFFFF). We only need to recognise the thumbnail class
// so we don't mistake a preview for the second view.
constexpr uint32_t kMpTypeMask = 0x00FFFFFFu;
constexpr uint32_t kMpTypeClassMask = 0x00FF0000u;
constexpr uint32_t kMpTypeClassThumb = 0x00010000u;  // 0x010001 / 0x010002 large thumbnails

bool ReadFile(const std::string& path, std::vector<uint8_t>& out, size_t maxBytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff len = f.tellg();
    if (len <= 0) return false;
    f.seekg(0, std::ios::beg);
    const size_t want = std::min((size_t)len, maxBytes);
    out.resize(want);
    f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)want);
    return (size_t)f.gcount() == want;
}

// Bounds-checked, byte-order-aware reads into the MPF TIFF block.
struct Tiff {
    const uint8_t* b = nullptr;
    size_t n = 0;        // bytes available from `base` to end of buffer
    size_t base = 0;     // file offset of the TIFF header — ALL offsets are relative to it
    bool little = true;

    bool U16(size_t rel, uint16_t& out) const {
        if (rel + 2 > n) return false;
        const uint8_t* p = b + base + rel;
        out = little ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]);
        return true;
    }
    bool U32(size_t rel, uint32_t& out) const {
        if (rel + 4 > n) return false;
        const uint8_t* p = b + base + rel;
        out = little ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                        ((uint32_t)p[3] << 24))
                     : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                        ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
        return true;
    }
};

// Walk the JPEG marker chain looking for APP2 "MPF\0". On success `tiffBase` is the file
// offset of the byte just after "MPF\0". Stops at SOS — everything past it is entropy
// data, and scanning that for a marker pattern would produce false hits.
bool FindMpfSegment(const std::vector<uint8_t>& b, size_t& tiffBase) {
    const size_t n = b.size();
    if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;   // not a JPEG (no SOI)
    size_t pos = 2;
    while (pos + 2 <= n) {
        if (b[pos] != 0xFF) { ++pos; continue; }                // resync
        const uint8_t marker = b[pos + 1];
        if (marker == 0xFF) { ++pos; continue; }                // fill byte
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;                                           // standalone, no payload
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) return false;      // EOI / SOS: give up
        if (pos + 4 > n) return false;
        const size_t segLen = ((size_t)b[pos + 2] << 8) | b[pos + 3];
        if (segLen < 2) return false;
        const size_t segStart = pos + 4;
        const size_t segEnd = pos + 2 + segLen;
        if (segEnd > n) return false;
        if (marker == 0xE2 && segEnd - segStart >= 4 &&
            std::memcmp(b.data() + segStart, "MPF\0", 4) == 0) {
            tiffBase = segStart + 4;
            return true;
        }
        pos = segEnd;
    }
    return false;
}

struct MpEntry {
    uint32_t type = 0;      // MP Type code (attribute & 0x00FFFFFF)
    uint32_t size = 0;      // image size in bytes
    uint32_t offset = 0;    // relative to the TIFF base; 0 means "file offset 0"
};

// Parse the MP Index IFD into a list of entries. Returns false on any malformed or
// out-of-range structure.
bool ParseMpEntries(const std::vector<uint8_t>& b, size_t tiffBase,
                    std::vector<MpEntry>& out) {
    Tiff t;
    t.b = b.data();
    t.base = tiffBase;
    if (tiffBase >= b.size()) return false;
    t.n = b.size() - tiffBase;

    if (t.n < 8) return false;
    const uint8_t o0 = b[tiffBase], o1 = b[tiffBase + 1];
    if (o0 == 0x49 && o1 == 0x49) t.little = true;
    else if (o0 == 0x4D && o1 == 0x4D) t.little = false;
    else return false;

    uint16_t magic = 0;
    uint32_t ifdOff = 0;
    if (!t.U16(2, magic) || magic != 42) return false;
    if (!t.U32(4, ifdOff)) return false;

    uint16_t count = 0;
    if (!t.U16(ifdOff, count) || count == 0) return false;
    // 12 bytes per entry plus the trailing next-IFD pointer.
    if ((size_t)ifdOff + 2 + (size_t)count * 12 + 4 > t.n) return false;

    uint32_t numImages = 0, mpEntryOff = 0, mpEntryCount = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const size_t e = (size_t)ifdOff + 2 + (size_t)i * 12;
        uint16_t tag = 0;
        uint32_t cnt = 0, val = 0;
        if (!t.U16(e, tag) || !t.U32(e + 4, cnt) || !t.U32(e + 8, val)) return false;
        if (tag == 0xB001) numImages = val;              // LONG, count 1 -> inline
        else if (tag == 0xB002) { mpEntryOff = val; mpEntryCount = cnt; }
    }
    if (numImages < 2) return false;                     // not a multi-picture file
    if (mpEntryCount < 32) return false;                 // < 2 entries' worth of bytes
    // Trust the smaller of the two counts; a disagreeing header is not worth guessing at.
    size_t entries = mpEntryCount / 16;
    if (entries > numImages) entries = numImages;
    if ((size_t)mpEntryOff + entries * 16 > t.n) return false;

    out.clear();
    out.reserve(entries);
    for (size_t i = 0; i < entries; ++i) {
        const size_t e = (size_t)mpEntryOff + i * 16;
        uint32_t attr = 0, sz = 0, off = 0;
        if (!t.U32(e, attr) || !t.U32(e + 4, sz) || !t.U32(e + 8, off)) return false;
        out.push_back(MpEntry{attr & kMpTypeMask, sz, off});
    }
    return out.size() >= 2;
}

} // namespace

bool MpoLoader::IsMpo(const std::string& path) {
    std::vector<uint8_t> head;
    if (!ReadFile(path, head, kSniffBytes)) return false;
    size_t tiffBase = 0;
    return FindMpfSegment(head, tiffBase);
}

MpoResult MpoLoader::Load(const std::string& path) {
    MpoResult r;

    std::vector<uint8_t> buf;
    if (!ReadFile(path, buf, (size_t)-1)) {
        LOG_WARN("MpoLoader: cannot read '%s'", path.c_str());
        return r;
    }
    size_t tiffBase = 0;
    if (!FindMpfSegment(buf, tiffBase)) return r;   // plain JPEG: silent, caller falls back

    std::vector<MpEntry> entries;
    if (!ParseMpEntries(buf, tiffBase, entries)) {
        LOG_WARN("MpoLoader: '%s' has an MPF segment but no usable MP index", path.c_str());
        return r;
    }

    // Second view: the first non-thumbnail entry after the primary. Do NOT require the
    // Multi-Frame Disparity type (0x020002) — real cameras are inconsistent about it, and
    // the dimension check below is the reliable filter.
    size_t pick = 0;
    for (size_t i = 1; i < entries.size(); ++i) {
        if ((entries[i].type & kMpTypeClassMask) == kMpTypeClassThumb) continue;
        if (entries[i].size == 0 || entries[i].offset == 0) continue;
        const size_t start = tiffBase + entries[i].offset;
        if (start >= buf.size() || start + entries[i].size > buf.size()) continue;
        pick = i;
        break;
    }
    if (pick == 0) {
        LOG_INFO("MpoLoader: '%s' MPF index has no usable second view (%zu entries)",
                 path.c_str(), entries.size());
        return r;
    }

    // The primary image starts at file offset 0 and is a normal JPEG; stb stops at its
    // EOI, so handing it the whole buffer is both correct and avoids trusting entry[0].
    DecodedImage left = ImageDecoder::LoadFromMemory(buf.data(), buf.size(), "MPO[1]");
    const size_t start = tiffBase + entries[pick].offset;
    DecodedImage right =
        ImageDecoder::LoadFromMemory(buf.data() + start, entries[pick].size, "MPO[2]");
    if (!left.Valid() || !right.Valid()) {
        LOG_WARN("MpoLoader: '%s' view decode failed", path.c_str());
        return r;
    }
    if (left.width != right.width || left.height != right.height) {
        LOG_WARN("MpoLoader: '%s' view size mismatch (%dx%d vs %dx%d) — not a stereo pair",
                 path.c_str(), left.width, left.height, right.width, right.height);
        return r;
    }

    r.image = ComposeSbs(left, right);
    if (!r.image.Valid()) {
        LOG_WARN("MpoLoader: '%s' SBS compose failed", path.c_str());
        return r;
    }
    r.layout = StereoLayout::SbsFull;
    r.ok = true;
    r.stereo = true;
    // MPO carries no baked convergence, so the estimate is always what Backspace applies.
    r.autoConvergence = EstimateAutoConvergence(left, right);
    LOG_INFO("MpoLoader: '%s' stereo MPO %dx%d SBS (per-eye %dx%d), MPType=0x%06X, "
             "auto convergence %+.4f",
             path.c_str(), r.image.width, r.image.height, left.width, left.height,
             entries[pick].type, (double)r.autoConvergence);
    return r;
}

} // namespace mp
