// SPDX-License-Identifier: Apache-2.0
#include "SpatialMedia.h"

#include <cstring>

namespace mp::spatial {

namespace {

uint32_t Be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
uint16_t Be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// One ISO-BMFF box header at [pos, end). On success `type` is the fourcc, `body` the first
// payload byte and `next` the byte after the box. A box that claims to run past `end` is
// malformed and stops the walk.
bool ReadBox(const uint8_t* d, size_t pos, size_t end, char type[5], size_t& body, size_t& next) {
    if (pos > end || end - pos < 8) return false;
    uint64_t size = Be32(d + pos);
    std::memcpy(type, d + pos + 4, 4);
    type[4] = 0;
    size_t hdr = 8;
    if (size == 1) {                // 64-bit largesize
        if (end - pos < 16) return false;
        size = (uint64_t)Be32(d + pos + 8) << 32 | Be32(d + pos + 12);
        hdr = 16;
    } else if (size == 0) {         // runs to the end of the enclosing container
        size = end - pos;
    }
    if (size < hdr || size > end - pos) return false;
    body = pos + hdr;
    next = pos + (size_t)size;
    return true;
}

void ParseGrpl(const uint8_t* d, size_t pos, size_t end, HeifStereoPair& out) {
    char t[5];
    size_t body = 0, next = 0;
    while (ReadBox(d, pos, end, t, body, next)) {
        // ster is an EntityToGroupBox: FullBox (version+flags), group_id, num_entities,
        // then num_entities entity ids.
        if (!out.hasPair && std::memcmp(t, "ster", 4) == 0 && next - body >= 12) {
            const uint32_t n = Be32(d + body + 8);
            if (n >= 2 && (next - body - 12) / 4 >= n) {
                out.leftId = Be32(d + body + 12);
                out.rightId = Be32(d + body + 16);
                out.hasPair = out.leftId != out.rightId;
            }
        }
        pos = next;
    }
}

void ParseMeta(const uint8_t* d, size_t pos, size_t end, HeifStereoPair& out) {
    char t[5];
    size_t body = 0, next = 0;
    while (ReadBox(d, pos, end, t, body, next)) {
        if (std::memcmp(t, "pitm", 4) == 0 && next - body >= 6) {
            const uint8_t version = d[body];
            if (version == 0) {
                out.primaryId = Be16(d + body + 4);
                out.hasPrimary = true;
            } else if (next - body >= 8) {
                out.primaryId = Be32(d + body + 4);
                out.hasPrimary = true;
            }
        } else if (std::memcmp(t, "grpl", 4) == 0) {
            ParseGrpl(d, body, next, out);
        }
        pos = next;
    }
}

}  // namespace

HeifStereoPair ParseHeifStereoPair(const uint8_t* data, size_t size) {
    HeifStereoPair out;
    if (!data) return out;
    char t[5];
    size_t pos = 0, body = 0, next = 0;
    while (ReadBox(data, pos, size, t, body, next)) {
        if (std::memcmp(t, "meta", 4) == 0) {
            // meta is a FullBox: skip version + flags before its children.
            if (next - body >= 4) ParseMeta(data, body + 4, next, out);
            break;
        }
        pos = next;
    }
    return out;
}

bool LooksLikeHeif(const uint8_t* data, size_t size) {
    char t[5];
    size_t body = 0, next = 0;
    if (!data || !ReadBox(data, 0, size, t, body, next) || std::memcmp(t, "ftyp", 4) != 0) return false;
    static const char* kBrands[] = {"heic", "heix", "heim", "heis", "hevc", "hevx",
                                    "mif1", "mif2", "msf1", "avif", "avis"};
    auto isHeifBrand = [&](const uint8_t* b) {
        for (const char* k : kBrands)
            if (std::memcmp(b, k, 4) == 0) return true;
        return false;
    };
    if (next - body < 8) return false;
    if (isHeifBrand(data + body)) return true;              // major brand
    for (size_t p = body + 8; p + 4 <= next; p += 4)         // compatible brands
        if (isHeifBrand(data + p)) return true;
    return false;
}

}  // namespace mp::spatial
