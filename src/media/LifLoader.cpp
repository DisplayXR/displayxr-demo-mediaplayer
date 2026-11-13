// SPDX-License-Identifier: Apache-2.0
#include "LifLoader.h"

#include "DepthWarp.h"
#include "Log.h"
#include "StereoCompose.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>
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
    // Camera x, used only to order left/right. Modern 5.x: position[0] (or
    // camera_data.position[0]); classic Leia: xLocation. Default 0.
    auto pick = [](const json& arr) -> float {
        if (arr.is_array() && !arr.empty() && arr[0].is_number())
            return arr[0].get<float>();
        return 0.0f;
    };
    if (view.contains("position")) return pick(view["position"]);
    if (view.contains("camera_data") && view["camera_data"].is_object() &&
        view["camera_data"].contains("position"))
        return pick(view["camera_data"]["position"]);
    if (view.contains("xLocation") && view["xLocation"].is_number())
        return view["xLocation"].get<float>();
    return 0.0f;
}

ViewRef ResolveView(const json& view, const std::vector<uint8_t>& buf,
                    const std::vector<Field>& fields) {
    ViewRef ref;
    // RGB blob id: modern 5.x nests it under "image"/"albedo".blob_id; classic Leia
    // puts it directly on the view as "albedoId". -1 means the base JPEG is this view.
    int64_t blobId = 0;
    bool haveBlob = false;
    const json* img = nullptr;
    if (view.contains("image") && view["image"].is_object()) img = &view["image"];
    else if (view.contains("albedo") && view["albedo"].is_object()) img = &view["albedo"];
    if (img && img->contains("blob_id") && (*img)["blob_id"].is_number_integer()) {
        blobId = (*img)["blob_id"].get<int64_t>();
        haveBlob = true;
    } else if (view.contains("albedoId") && view["albedoId"].is_number_integer()) {
        blobId = view["albedoId"].get<int64_t>();
        haveBlob = true;
    }
    if (!haveBlob) return ref;

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

// (ComposeSbs + EstimateAutoConvergence moved to StereoCompose.{h,cpp} when the MPO
// container loader landed — #45. Same code, now shared by both containers.)

// Decode the base JPEG (the file is a valid JPEG even when it isn't a LIF) → flat 2D.
LifResult MonoFallback(const std::vector<uint8_t>& buf, const std::string& path) {
    LifResult r;
    r.image = ImageDecoder::LoadFromMemory(buf.data(), buf.size(), path.c_str());
    r.layout = StereoLayout::Mono;
    r.stereo = false;
    r.ok = r.image.Valid();
    return r;
}

// The parallax of a one-view LIF's whole disparity range, as a fraction of the width. A
// classic Leia LIF has only "gain", whose scale isn't published: kGainToParallax is matched
// by eye on the panel, kDefaultParallax is for a null gain.
constexpr float kGainToParallax = 0.06f;
constexpr float kDefaultParallax = 0.015f;
// LIF 5.3 files from the python LIF encoder carry a stereo baseline of 1.0, which reads as
// about 10% here, where the C++ encoder's 0.13-0.15 gives 1.3-1.5%.
constexpr float kMaxParallax = 0.02f;

bool Num(const json& o, const char* key, float& v) {
    if (!o.contains(key) || !o[key].is_number()) return false;
    v = o[key].get<float>();
    return true;
}

// LIF 5.x: "inv_z_map" (legacy "disparity"), with the map's min/max inverse depth.
const json* InvZMap(const json& view) {
    for (const char* k : {"inv_z_map", "disparity"})
        if (view.contains(k) && view[k].is_object()) return &view[k];
    return nullptr;
}

// The disparity map blob: 5.x inv_z_map.blob_id, or classic Leia "disparityId".
const Field* DisparityField(const json& view, const std::vector<Field>& fields) {
    const json* m = InvZMap(view);
    const json* id = (m && m->contains("blob_id")) ? &(*m)["blob_id"]
                     : view.contains("disparityId") ? &view["disparityId"] : nullptr;
    if (!id || !id->is_number_integer() || id->get<int64_t>() < 0) return nullptr;
    return FindField(fields, (uint32_t)id->get<int64_t>());
}

float OneViewParallax(const json& root, const json& view) {
    // 5.x: a pixel's disparity is focal_px * baseline * invZ, and the map spans max..min.
    const json* m = InvZMap(view);
    float mn = 0, mx = 0, focal = 0, width = 0, baseline = 0, gain = 0;
    if (m && Num(*m, "min", mn) && Num(*m, "max", mx) && Num(view, "focal_px", focal) &&
        Num(view, "width_px", width) && width > 0 && root.contains("stereo_render_data") &&
        Num(root["stereo_render_data"], "baseline", baseline))
        return focal * baseline * (mn - mx) / width;
    if (Num(root, "gain", gain) && gain > 0) return kGainToParallax * gain;
    return kDefaultParallax;
}

// The parallax DepthWarp::MakeSbs gets for a one-view LIF, and whether its map must be
// flipped first: inv_z_map min < max puts the far end at 255, where DepthWarp's occlusion
// takes 255 as the nearest. Shared by the player (LoadOneView) and the 3D grid's thumbnail
// pairs (FindDepthView), so both show the same depth.
void ResolveParallax(const json& root, const json& view, float& parallax, bool& invert) {
    parallax = OneViewParallax(root, view);
    invert = parallax < 0;
    if (invert) parallax = -parallax;
    parallax = std::min(parallax, kMaxParallax);
}

// One byte every `step` of an sw x sh plane, at w x h: sampled nearest when the sizes differ.
std::vector<uint8_t> DepthPlane(const uint8_t* src, int step, int sw, int sh, int w, int h) {
    std::vector<uint8_t> d((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + (size_t)(y * sh / h) * sw * step;
        for (int x = 0; x < w; ++x) d[(size_t)y * w + x] = row[(size_t)(x * sw / w) * step];
    }
    return d;
}

// #180: converges by the same measurement as a two-view LIF without a convergence.
LifResult LoadOneView(const json& root, const std::vector<uint8_t>& buf,
                      const std::vector<Field>& fields, const std::string& path) {
    const json& view = root["views"][0];
    const ViewRef img = ResolveView(view, buf, fields);
    const Field* disp = DisparityField(view, fields);
    if (!img.valid || !disp) {
        LOG_INFO("LifLoader: '%s' has 1 view and no disparity map, flat 2D", path.c_str());
        return MonoFallback(buf, path);
    }
    // The map's first channel is taken on its decode thread, overlapping the photo's decode.
    std::vector<uint8_t> depth;
    int mw = 0, mh = 0;
    std::thread mdec([&] {
        const DecodedImage map =
            ImageDecoder::LoadFromMemory(buf.data() + disp->offset, disp->size, "LIF[disparity]");
        if (!map.Valid()) return;
        mw = map.width;
        mh = map.height;
        depth = DepthPlane(map.pixels.data(), 4, mw, mh, mw, mh);
    });
    const DecodedImage base = ImageDecoder::LoadFromMemory(img.data, img.size, "LIF[view]");
    mdec.join();
    if (!base.Valid() || depth.empty()) {
        LOG_WARN("LifLoader: '%s' view or disparity map decode failed, flat 2D", path.c_str());
        return MonoFallback(buf, path);
    }
    if (mw != base.width || mh != base.height)
        depth = DepthPlane(depth.data(), 1, mw, mh, base.width, base.height);
    float parallax = 0.0f;
    bool invert = false;
    ResolveParallax(root, view, parallax, invert);
    if (invert)
        for (uint8_t& d : depth) d = 255 - d;
    LifResult r;
    r.image = DepthWarp::MakeSbs(base, depth, parallax);
    r.layout = StereoLayout::SbsFull;
    r.ok = r.stereo = true;
    r.autoConvergence = EstimateAutoConvergence(r.image);
    LOG_INFO("LifLoader: '%s' one-view LIF %dx%d, pair from its disparity map (parallax %.2f%%), "
             "convergence=%+.4f (auto estimate)",
             path.c_str(), base.width, base.height, parallax * 100.0f, r.autoConvergence);
    return r;
}

} // namespace

bool LifLoader::IsLif(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff len = f.tellg();
    if (len < 2) return false;
    f.seekg(len - 2);
    unsigned char tail[2] = {0, 0};
    f.read(reinterpret_cast<char*>(tail), 2);
    return (bool)f && tail[0] == 0x1E && tail[1] == 0x1A;
}

namespace {

// A LIF's field table and JSON read through `readAt`: the trailer, the field headers and the
// JSON only (a few KB of a 20 MB file). For FindViews and FindDepthView.
struct MetaSpan {
    uint32_t type = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
};
struct Meta {
    uint64_t region = 0;  // where the metadata region starts: the base JPEG ends here
    std::vector<MetaSpan> fields;
    json root;
    const MetaSpan* Find(uint32_t type) const {
        for (const MetaSpan& f : fields)
            if (f.type == type) return &f;
        return nullptr;
    }
};

bool ReadMeta(const LifLoader::ReadAtFn& readAt, uint64_t fileSize, Meta& meta) {
    constexpr uint32_t kMaxFields = 256;
    constexpr uint32_t kMaxJson = 1u << 20;
    auto u32 = [&](uint64_t at, uint32_t& v) {
        uint8_t b[4];
        if (!readAt(at, b, 4)) return false;
        v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        return true;
    };
    uint8_t tail[2] = {0, 0};
    if (fileSize < 6 || !readAt(fileSize - 2, tail, 2) || tail[0] != 0x1E || tail[1] != 0x1A) return false;
    uint32_t regionOffset = 0, count = 0;
    if (!u32(fileSize - 6, regionOffset) || regionOffset == 0 || regionOffset > fileSize) return false;
    meta.region = fileSize - regionOffset;
    if (!u32(meta.region, count) || count > kMaxFields) return false;
    uint64_t cur = meta.region + 4;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t type = 0, size = 0;
        if (!u32(cur, type) || !u32(cur + 4, size)) return false;
        cur += 8;
        if (cur + size > fileSize) return false;
        meta.fields.push_back(MetaSpan{type, cur, size});
        cur += size;
    }
    const MetaSpan* m = meta.Find(kJsonMetaNew);
    if (!m) m = meta.Find(kJsonMetaOld);
    if (!m || m->size == 0 || m->size > kMaxJson) return false;
    std::vector<uint8_t> text(m->size);
    if (!readAt(m->offset, text.data(), text.size())) return false;
    try {
        meta.root = json::parse(text.begin(), text.end());
    } catch (const std::exception&) {
        return false;
    }
    return meta.root.contains("views") && meta.root["views"].is_array() && !meta.root["views"].empty();
}

// A view's RGB image span, by the same blob-id rules as ResolveView: 5.x
// image/albedo.blob_id, classic albedoId; -1 is the base JPEG.
bool ResolveSpan(const Meta& meta, const json& view, LifLoader::ViewSpan& out) {
    const json* img = nullptr;
    if (view.contains("image") && view["image"].is_object()) img = &view["image"];
    else if (view.contains("albedo") && view["albedo"].is_object()) img = &view["albedo"];
    int64_t blobId = 0;
    if (img && img->contains("blob_id") && (*img)["blob_id"].is_number_integer()) {
        blobId = (*img)["blob_id"].get<int64_t>();
    } else if (view.contains("albedoId") && view["albedoId"].is_number_integer()) {
        blobId = view["albedoId"].get<int64_t>();
    } else {
        return false;
    }
    if (blobId == -1) {
        out = LifLoader::ViewSpan{0, meta.region, true};
        return true;
    }
    const MetaSpan* f = blobId >= 0 ? meta.Find((uint32_t)blobId) : nullptr;
    if (!f) return false;
    out = LifLoader::ViewSpan{f->offset, f->size, false};
    return true;
}

} // namespace

bool LifLoader::FindViews(const ReadAtFn& readAt, uint64_t fileSize, ViewSpan& left, ViewSpan& right) {
    Meta meta;
    if (!ReadMeta(readAt, fileSize, meta) || meta.root["views"].size() < 2) return false;
    ViewSpan a, b;
    const json& va = meta.root["views"][0];
    const json& vb = meta.root["views"][1];
    if (!ResolveSpan(meta, va, a) || !ResolveSpan(meta, vb, b)) return false;
    const float ax = ViewPosX(va), bx = ViewPosX(vb);
    left = ax <= bx ? a : b;
    right = ax <= bx ? b : a;
    return true;
}

bool LifLoader::FindDepthView(const ReadAtFn& readAt, uint64_t fileSize, DepthViewSpan& out) {
    Meta meta;
    if (!ReadMeta(readAt, fileSize, meta) || meta.root["views"].size() != 1) return false;
    const json& view = meta.root["views"][0];
    if (!ResolveSpan(meta, view, out.image)) return false;
    // The disparity map blob, as DisparityField: 5.x inv_z_map.blob_id, classic disparityId.
    const json* m = InvZMap(view);
    const json* id = (m && m->contains("blob_id")) ? &(*m)["blob_id"]
                     : view.contains("disparityId") ? &view["disparityId"] : nullptr;
    if (!id || !id->is_number_integer() || id->get<int64_t>() < 0) return false;
    const MetaSpan* map = meta.Find((uint32_t)id->get<int64_t>());
    if (!map || map->size == 0) return false;
    out.mapOffset = map->offset;
    out.mapSize = map->size;
    ResolveParallax(meta.root, view, out.parallax, out.invert);
    return true;
}

bool LifLoader::IsLif(const ReadAtFn& readAt, uint64_t fileSize) {
    unsigned char tail[2] = {0, 0};
    return fileSize >= 2 && readAt(fileSize - 2, tail, 2) && tail[0] == 0x1E && tail[1] == 0x1A;
}

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
        if (n == 1) return LoadOneView(root, buf, fields, path);
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

    // The views decode at once: about 40% off an 8K LIF's load on the NP02J.
    DecodedImage rimg;
    std::thread rdec([&] { rimg = ImageDecoder::LoadFromMemory(right.data, right.size, "LIF[right]"); });
    DecodedImage limg = ImageDecoder::LoadFromMemory(left.data, left.size, "LIF[left]");
    rdec.join();
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
    LifResult r;
    r.image = ComposeSbs(limg, rimg);
    if (!r.image.Valid()) {
        LOG_WARN("LifLoader: '%s' SBS compose failed — flat 2D", path.c_str());
        return MonoFallback(buf, path);
    }
    r.layout = StereoLayout::SbsFull;
    r.stereo = true;
    r.ok = true;
    // Baked reconvergence: the LIF's normalized horizontal shift that sets the intended
    // zero-disparity plane. Top-level "convergence" (classic Leia). When the field is
    // absent (or null), estimate one from the pair; the player applies it by default.
    r.hasConvergence = (root.contains("convergence") && root["convergence"].is_number());
    if (r.hasConvergence) {
        r.convergence = root["convergence"].get<float>();
    } else {
        r.autoConvergence = EstimateAutoConvergence(r.image);
    }
    LOG_INFO("LifLoader: '%s' stereo LIF %dx%d SBS (per-eye %dx%d), convergence=%+.4f%s",
             path.c_str(), r.image.width, r.image.height, w, h,
             r.hasConvergence ? r.convergence : r.autoConvergence,
             r.hasConvergence ? "" : " (MISSING — auto estimate)");
    return r;
}

} // namespace mp
