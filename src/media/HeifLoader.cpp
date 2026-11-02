// SPDX-License-Identifier: Apache-2.0
#include "HeifLoader.h"

#include "Log.h"
#include "SpatialMedia.h"
#include "StereoCompose.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#if defined(MEDIAPLAYER_WITH_FFMPEG)
extern "C" {
#include <libavutil/display.h>
#include <libswscale/swscale.h>
}
#include "SpatialFFmpeg.h"
#endif

namespace mp {

bool HeifLoader::IsHeifPath(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (const char* e : {".heic", ".heif", ".hif"}) {
        const size_t n = std::strlen(e);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0) return true;
    }
    return false;
}

#if defined(MEDIAPLAYER_WITH_FFMPEG) && MP_HAVE_FF_SPATIAL

bool HeifLoader::Available() { return true; }

namespace {

// Any decoded software frame -> tightly packed RGBA8, honouring the frame's matrix + range.
bool ToRGBA(const AVFrame* f, DecodedImage& out) {
    const AVPixelFormat pf = (AVPixelFormat)f->format;
    SwsContext* sws = sws_getContext(f->width, f->height, pf, f->width, f->height, AV_PIX_FMT_RGBA, SWS_BICUBIC,
                                     nullptr, nullptr, nullptr);
    if (!sws) return false;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf);
    if (d && !(d->flags & AV_PIX_FMT_FLAG_RGB)) {
        const int cs = (f->colorspace == AVCOL_SPC_BT709) ? SWS_CS_ITU709
                       : (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL)
                           ? SWS_CS_BT2020
                           : SWS_CS_ITU601;
        const bool full = f->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P ||
                          pf == AV_PIX_FMT_YUVJ422P || pf == AV_PIX_FMT_YUVJ444P;
        sws_setColorspaceDetails(sws, sws_getCoefficients(cs), full ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                 0, 1 << 16, 1 << 16);
    }
    out.width = f->width;
    out.height = f->height;
    out.pixels.assign((size_t)f->width * f->height * 4, 255);
    uint8_t* dst[4] = {out.pixels.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {f->width * 4, 0, 0, 0};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dstStride);
    sws_freeContext(sws);
    return true;
}

// Rotate an RGBA image clockwise by a multiple of 90 degrees.
DecodedImage RotateCW(const DecodedImage& in, int quarterTurns) {
    quarterTurns = ((quarterTurns % 4) + 4) % 4;
    if (quarterTurns == 0) return in;
    DecodedImage out;
    const bool swap = quarterTurns % 2 == 1;
    out.width = swap ? in.height : in.width;
    out.height = swap ? in.width : in.height;
    out.pixels.resize(in.pixels.size());
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            int dx, dy;
            if (quarterTurns == 1) { dx = in.height - 1 - y; dy = x; }
            else if (quarterTurns == 2) { dx = in.width - 1 - x; dy = in.height - 1 - y; }
            else { dx = y; dy = in.width - 1 - x; }
            std::memcpy(&out.pixels[((size_t)dy * out.width + dx) * 4], &in.pixels[((size_t)y * in.width + x) * 4], 4);
        }
    }
    return out;
}

// The display matrix (HEIF irot) as clockwise quarter turns, 0 when absent.
int QuarterTurnsCW(const AVPacketSideData* sd, int nb) {
    const AVPacketSideData* m = av_packet_side_data_get(sd, nb, AV_PKT_DATA_DISPLAYMATRIX);
    if (!m || m->size < 9 * 4) return 0;
    const double ccw = av_display_rotation_get(reinterpret_cast<const int32_t*>(m->data));
    if (std::isnan(ccw)) return 0;
    return (int)std::lround(-ccw / 90.0);  // av_display_rotation_get is counter-clockwise
}

class Reader {
public:
    explicit Reader(const std::string& path) : path_(path) {}
    ~Reader() {
        for (auto& kv : packets_) av_packet_free(&kv.second);
        if (fmt_) avformat_close_input(&fmt_);
    }

    bool Open() {
        if (avformat_open_input(&fmt_, path_.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt_, nullptr) < 0) return false;
        // One sample per item: read them all up front (a photo, so a few MB at most).
        AVPacket* pkt = av_packet_alloc();
        while (pkt && av_read_frame(fmt_, pkt) >= 0) {
            if (!packets_.count(pkt->stream_index)) {
                packets_[pkt->stream_index] = av_packet_clone(pkt);
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        return true;
    }

    // The item `id` (a grid or a single coded image) as upright RGBA.
    bool DecodeItem(uint32_t id, DecodedImage& out) {
        for (unsigned g = 0; g < fmt_->nb_stream_groups; ++g) {
            const AVStreamGroup* stg = fmt_->stream_groups[g];
            if (stg->type == AV_STREAM_GROUP_PARAMS_TILE_GRID && (uint32_t)stg->id == id) return DecodeGrid(stg, out);
        }
        for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
            const AVStream* st = fmt_->streams[i];
            if ((uint32_t)st->id == id) {
                DecodedImage img;
                if (!DecodeStream((int)i, img)) return false;
                out = RotateCW(img, QuarterTurnsCW(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data));
                return true;
            }
        }
        LOG_WARN("HeifLoader: '%s' has no image item %u", path_.c_str(), id);
        return false;
    }

    // What an ordinary viewer shows: the default (primary) grid or stream.
    bool DecodeDefault(DecodedImage& out) {
        for (unsigned g = 0; g < fmt_->nb_stream_groups; ++g) {
            const AVStreamGroup* stg = fmt_->stream_groups[g];
            if (stg->type == AV_STREAM_GROUP_PARAMS_TILE_GRID && (stg->disposition & AV_DISPOSITION_DEFAULT))
                return DecodeItem((uint32_t)stg->id, out);
        }
        const int best = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        return best >= 0 && DecodeItem((uint32_t)fmt_->streams[best]->id, out);
    }

private:
    bool DecodeStream(int index, DecodedImage& out) {
        auto it = packets_.find(index);
        if (it == packets_.end()) return false;
        const AVStream* st = fmt_->streams[index];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) return false;
        AVCodecContext* c = avcodec_alloc_context3(codec);
        bool ok = false;
        AVFrame* f = av_frame_alloc();
        if (c && f && avcodec_parameters_to_context(c, st->codecpar) >= 0 && avcodec_open2(c, codec, nullptr) >= 0 &&
            avcodec_send_packet(c, it->second) >= 0) {
            avcodec_send_packet(c, nullptr);
            if (avcodec_receive_frame(c, f) >= 0) ok = ToRGBA(f, out);
        }
        av_frame_free(&f);
        avcodec_free_context(&c);
        return ok;
    }

    bool DecodeGrid(const AVStreamGroup* stg, DecodedImage& out) {
        const AVStreamGroupTileGrid* tg = stg->params.tile_grid;
        if (!tg || tg->coded_width <= 0 || tg->coded_height <= 0 || tg->coded_width > 32768 ||
            tg->coded_height > 32768)
            return false;
        DecodedImage canvas;
        canvas.width = tg->coded_width;
        canvas.height = tg->coded_height;
        canvas.pixels.assign((size_t)canvas.width * canvas.height * 4, 0);
        for (unsigned t = 0; t < tg->nb_tiles; ++t) {
            const unsigned idx = tg->offsets[t].idx;
            if (idx >= stg->nb_streams) return false;
            DecodedImage tile;
            if (!DecodeStream(stg->streams[idx]->index, tile)) return false;
            const int ox = tg->offsets[t].horizontal, oy = tg->offsets[t].vertical;
            for (int y = 0; y < tile.height && oy + y < canvas.height; ++y) {
                if (oy + y < 0 || ox >= canvas.width) continue;
                const int x0 = std::max(0, -ox);
                const int w = std::min(tile.width, canvas.width - ox) - x0;
                if (w <= 0) continue;
                std::memcpy(&canvas.pixels[((size_t)(oy + y) * canvas.width + ox + x0) * 4],
                            &tile.pixels[((size_t)y * tile.width + x0) * 4], (size_t)w * 4);
            }
        }
        // Crop to the grid's output rectangle.
        const int cx = std::clamp(tg->horizontal_offset, 0, canvas.width);
        const int cy = std::clamp(tg->vertical_offset, 0, canvas.height);
        const int cw = std::clamp(tg->width > 0 ? tg->width : canvas.width, 1, canvas.width - cx);
        const int ch = std::clamp(tg->height > 0 ? tg->height : canvas.height, 1, canvas.height - cy);
        DecodedImage img;
        img.width = cw;
        img.height = ch;
        img.pixels.resize((size_t)cw * ch * 4);
        for (int y = 0; y < ch; ++y)
            std::memcpy(&img.pixels[(size_t)y * cw * 4], &canvas.pixels[((size_t)(cy + y) * canvas.width + cx) * 4],
                        (size_t)cw * 4);
        out = RotateCW(img, QuarterTurnsCW(tg->coded_side_data, tg->nb_coded_side_data));
        return true;
    }

    std::string path_;
    AVFormatContext* fmt_ = nullptr;
    std::map<int, AVPacket*> packets_;
};

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0 || n > (std::streamoff)512 * 1024 * 1024) return false;
    out.resize((size_t)n);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return (bool)f;
}

}  // namespace

HeifResult HeifLoader::Load(const std::string& path) {
    HeifResult r;
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) {
        LOG_ERROR("HeifLoader: cannot read '%s'", path.c_str());
        return r;
    }
    if (!spatial::LooksLikeHeif(bytes.data(), bytes.size())) {
        LOG_ERROR("HeifLoader: '%s' is not a HEIF file", path.c_str());
        return r;
    }
    const spatial::HeifStereoPair pair = spatial::ParseHeifStereoPair(bytes.data(), bytes.size());
    bytes.clear();
    bytes.shrink_to_fit();

    Reader rd(path);
    if (!rd.Open()) {
        LOG_ERROR("HeifLoader: FFmpeg cannot open '%s'", path.c_str());
        return r;
    }
    if (pair.hasPair) {
        DecodedImage left, right;
        if (rd.DecodeItem(pair.leftId, left) && rd.DecodeItem(pair.rightId, right)) {
            DecodedImage sbs = ComposeSbs(left, right);
            if (sbs.Valid()) {
                r.autoConvergence = EstimateAutoConvergence(left, right);
                r.image = std::move(sbs);
                r.layout = StereoLayout::SbsFull;
                r.ok = r.stereo = true;
                LOG_INFO("HeifLoader: '%s' stereo pair (ster: left item %u, right item %u), %dx%d per eye",
                         path.c_str(), pair.leftId, pair.rightId, left.width, left.height);
                return r;
            }
            LOG_WARN("HeifLoader: '%s' stereo views differ in size (%dx%d vs %dx%d); showing one flat", path.c_str(),
                     left.width, left.height, right.width, right.height);
        } else {
            LOG_WARN("HeifLoader: '%s' has a stereo group but an eye failed to decode; showing it flat",
                     path.c_str());
        }
    }
    const bool ok = pair.hasPrimary ? rd.DecodeItem(pair.primaryId, r.image) : rd.DecodeDefault(r.image);
    if (!ok && !(pair.hasPrimary && rd.DecodeDefault(r.image))) {
        LOG_ERROR("HeifLoader: no decodable image in '%s'", path.c_str());
        return r;
    }
    r.ok = r.image.Valid();
    LOG_INFO("HeifLoader: '%s' %s, %dx%d (flat)", path.c_str(), pair.hasPair ? "stereo pair unusable" : "no stereo group",
             r.image.width, r.image.height);
    return r;
}

#else  // no FFmpeg, or one older than 7.1 (no HEIF items)

bool HeifLoader::Available() { return false; }
HeifResult HeifLoader::Load(const std::string& path) {
    LOG_ERROR("HeifLoader: this build cannot read HEIF ('%s')", path.c_str());
    return HeifResult{};
}

#endif

} // namespace mp
