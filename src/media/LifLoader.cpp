// SPDX-License-Identifier: BSL-1.0
#include "LifLoader.h"

#include "Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace mp {

namespace {

using json = nlohmann::json;

// LIF trailer/field tables are big-endian regardless of host. Bounds-checked reads
// (return false on any out-of-range access) so a truncated/garbage file can't crash us.
bool ReadU16BE(const std::vector<uint8_t>& b, size_t off, uint16_t& out) {
    if (off + 2 > b.size()) return false;
    out = (uint16_t)((b[off] << 8) | b[off + 1]);
    return true;
}
bool ReadU32BE(const std::vector<uint8_t>& b, size_t off, uint32_t& out) {
    if (off + 4 > b.size()) return false;
    out = ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) |
          ((uint32_t)b[off + 2] << 8) | (uint32_t)b[off + 3];
    return true;
}

constexpr uint16_t kLifMagic = 0x1E1A;  // last two bytes of a LIF file
constexpr uint32_t kJsonMetaNew = 8;    // field type carrying the UTF-8 JSON metadata
constexpr uint32_t kJsonMetaOld = 7;    // legacy JSON field type

struct Field {
    uint32_t type = 0;
    size_t offset = 0;  // start of this field's data within the file buffer
    size_t size = 0;
};

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff len = f.tellg();
    if (len <= 0) return false;
    out.resize((size_t)len);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)len);
    return (bool)f;
}

// Parse the appended metadata region into a flat field list. Returns false if the
// file has no valid LIF trailer (i.e. it's a plain image, not a LIF container).
bool ParseFields(const std::vector<uint8_t>& buf, std::vector<Field>& fields) {
    const size_t full = buf.size();
    if (full < 6) return false;

    uint16_t magic = 0;
    if (!ReadU16BE(buf, full - 2, magic) || magic != kLifMagic) return false;

    uint32_t regionOffset = 0;
    if (!ReadU32BE(buf, full - 6, regionOffset)) return false;
    if (regionOffset == 0 || regionOffset > full) return false;

    size_t cur = full - regionOffset;
    uint32_t fieldCount = 0;
    if (!ReadU32BE(buf, cur, fieldCount)) return false;
    cur += 4;

    for (uint32_t i = 0; i < fieldCount; ++i) {
        uint32_t type = 0, size = 0;
        if (!ReadU32BE(buf, cur, type)) return false;
        cur += 4;
        if (!ReadU32BE(buf, cur, size)) return false;
        cur += 4;
        if (cur + size > full) return false;  // field data runs past EOF
        fields.push_back(Field{type, cur, size});
        cur += size;
    }
    return true;
}

const Field* FindField(const std::vector<Field>& fields, uint32_t type) {
    for (const auto& f : fields) {
        if (f.type == type) return &f;
    }
    return nullptr;
}

// A LIF view's RGB image is either the base JPEG (blob_id == -1, i.e. the whole file
// — stb stops at the JPEG EOI and ignores the appended trailer) or an embedded blob.
struct ViewRef {
    const uint8_t* data = nullptr;
    size_t size = 0;
    float posX = 0.0f;  // camera x (normalized) — used only to order left/right
    bool valid = false;
};

float ViewPosX(const json& view) {
    // position[0], or legacy camera_data.position[0]; default 0.
    auto pick = [](const json& arr) -> float {
        if (arr.is_array() && !arr.empty() && arr[0].is_number())
            return arr[0].get<float>();
        return 0.0f;
    };
    if (view.contains("position")) return pick(view["position"]);
    if (view.contains("camera_data") && view["camera_data"].is_object() &&
        view["camera_data"].contains("position"))
        return pick(view["camera_data"]["position"]);
    return 0.0f;
}

ViewRef ResolveView(const json& view, const std::vector<uint8_t>& buf,
                    const std::vector<Field>& fields) {
    ViewRef ref;
    // RGB lives under "image" (5.x) or legacy "albedo".
    const json* img = nullptr;
    if (view.contains("image") && view["image"].is_object()) img = &view["image"];
    else if (view.contains("albedo") && view["albedo"].is_object()) img = &view["albedo"];
    if (!img || !img->contains("blob_id") || !(*img)["blob_id"].is_number_integer())
        return ref;

    const int64_t blobId = (*img)["blob_id"].get<int64_t>();
    if (blobId == -1) {
        ref.data = buf.data();          // the base JPEG is this view's image
        ref.size = buf.size();
    } else {
        const Field* f = FindField(fields, (uint32_t)blobId);
        if (!f) return ref;
        ref.data = buf.data() + f->offset;
        ref.size = f->size;
    }
    ref.posX = ViewPosX(view);
    ref.valid = true;
    return ref;
}

// Decode the base JPEG (the file is a valid JPEG even when it isn't a LIF) → flat 2D.
LifResult MonoFallback(const std::vector<uint8_t>& buf, const std::string& path) {
    LifResult r;
    r.image = ImageDecoder::LoadFromMemory(buf.data(), buf.size(), path.c_str());
    r.layout = StereoLayout::Mono;
    r.stereo = false;
    r.ok = r.image.Valid();
    return r;
}

} // namespace

LifResult LifLoader::Load(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!ReadWholeFile(path, buf)) {
        LOG_ERROR("LifLoader: cannot read '%s'", path.c_str());
        return LifResult{};
    }

    std::vector<Field> fields;
    if (!ParseFields(buf, fields)) {
        LOG_WARN("LifLoader: '%s' has no LIF trailer — treating as flat 2D", path.c_str());
        return MonoFallback(buf, path);
    }

    const Field* metaField = FindField(fields, kJsonMetaNew);
    if (!metaField) metaField = FindField(fields, kJsonMetaOld);
    if (!metaField) {
        LOG_WARN("LifLoader: '%s' has no JSON metadata field — flat 2D", path.c_str());
        return MonoFallback(buf, path);
    }

    json root;
    try {
        root = json::parse(buf.begin() + metaField->offset,
                           buf.begin() + metaField->offset + metaField->size);
    } catch (const std::exception& e) {
        LOG_WARN("LifLoader: '%s' JSON parse failed (%s) — flat 2D", path.c_str(), e.what());
        return MonoFallback(buf, path);
    }

    if (!root.contains("views") || !root["views"].is_array() || root["views"].size() < 2) {
        const size_t n = root.contains("views") && root["views"].is_array()
                             ? root["views"].size() : 0;
        LOG_INFO("LifLoader: '%s' has %zu view(s); stereo needs 2 — flat 2D", path.c_str(), n);
        return MonoFallback(buf, path);
    }

    // Resolve the first two views' RGB blobs and order them left→right by camera x.
    ViewRef a = ResolveView(root["views"][0], buf, fields);
    ViewRef b = ResolveView(root["views"][1], buf, fields);
    if (!a.valid || !b.valid) {
        LOG_WARN("LifLoader: '%s' could not resolve both view images — flat 2D", path.c_str());
        return MonoFallback(buf, path);
    }
    const ViewRef& left = (a.posX <= b.posX) ? a : b;
    const ViewRef& right = (a.posX <= b.posX) ? b : a;

    DecodedImage limg = ImageDecoder::LoadFromMemory(left.data, left.size, "LIF[left]");
    DecodedImage rimg = ImageDecoder::LoadFromMemory(right.data, right.size, "LIF[right]");
    if (!limg.Valid() || !rimg.Valid()) {
        LOG_WARN("LifLoader: '%s' view decode failed — flat 2D", path.c_str());
        return MonoFallback(buf, path);
    }
    if (limg.width != rimg.width || limg.height != rimg.height) {
        LOG_WARN("LifLoader: '%s' view size mismatch (%dx%d vs %dx%d) — flat 2D",
                 path.c_str(), limg.width, limg.height, rimg.width, rimg.height);
        return MonoFallback(buf, path);
    }

    // Compose full-SBS RGBA: left view into the left half, right view into the right.
    const int w = limg.width, h = limg.height;
    const size_t rowBytes = (size_t)w * 4;
    LifResult r;
    r.image.width = w * 2;
    r.image.height = h;
    r.image.pixels.resize((size_t)w * 2 * h * 4);
    uint8_t* dst = r.image.pixels.data();
    for (int y = 0; y < h; ++y) {
        const size_t rowStart = (size_t)y * (size_t)(w * 2) * 4;
        std::copy_n(limg.pixels.data() + (size_t)y * rowBytes, rowBytes, dst + rowStart);
        std::copy_n(rimg.pixels.data() + (size_t)y * rowBytes, rowBytes,
                    dst + rowStart + rowBytes);
    }
    r.layout = StereoLayout::SbsFull;
    r.stereo = true;
    r.ok = true;
    LOG_INFO("LifLoader: '%s' stereo LIF composed to %dx%d SBS (per-eye %dx%d)",
             path.c_str(), r.image.width, r.image.height, w, h);
    return r;
}

} // namespace mp
