// SPDX-License-Identifier: Apache-2.0
#include "VideoThumbnail.h"

#include <algorithm>
#include <chrono>

#if defined(MEDIAPLAYER_WITH_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#endif

namespace mp {

#if !defined(MEDIAPLAYER_WITH_FFMPEG)

bool VideoThumbnail(const std::string&, uint32_t, uint32_t&, uint32_t&, std::vector<uint8_t>&, int) {
    return false;
}

#else

namespace {

struct Ctx {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    ~Ctx() {
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
    }
};

// Mean luma of a sparse sample of the first plane (YUV formats; a guess for the rest).
bool Dark(const AVFrame* f) {
    if (!f->data[0] || f->width <= 0 || f->height <= 0) return false;
    uint64_t acc = 0, n = 0;
    for (int y = f->height / 8; y < f->height; y += std::max(1, f->height / 16))
        for (int x = f->width / 8; x < f->width; x += std::max(1, f->width / 16)) {
            acc += f->data[0][(size_t)y * f->linesize[0] + x];
            ++n;
        }
    return n > 0 && acc / n < 24;
}

bool ToRgba(const AVFrame* f, uint32_t maxEdge, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba) {
    if (f->width <= 0 || f->height <= 0) return false;
    const double s = std::min(1.0, (double)maxEdge / (double)std::max(f->width, f->height));
    const int w = std::max(1, (int)(f->width * s + 0.5)), h = std::max(1, (int)(f->height * s + 0.5));
    SwsContext* sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, w, h, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return false;
    rgba.assign((size_t)w * h * 4, 0);
    uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {w * 4, 0, 0, 0};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dstStride);
    sws_freeContext(sws);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    width = (uint32_t)w;
    height = (uint32_t)h;
    return true;
}

} // namespace

bool VideoThumbnail(const std::string& path, uint32_t maxEdge, uint32_t& width, uint32_t& height,
                    std::vector<uint8_t>& rgba, int budgetMs) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    width = height = 0;
    rgba.clear();
    if (maxEdge == 0) maxEdge = 512;

    Ctx c;
    if (avformat_open_input(&c.fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(c.fmt, nullptr) < 0) return false;
#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVCodec* codec = nullptr;
#else
    AVCodec* codec = nullptr;
#endif
    const int stream = av_find_best_stream(c.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) return false;
    AVStream* st = c.fmt->streams[stream];
    c.dec = avcodec_alloc_context3(codec);
    if (!c.dec || avcodec_parameters_to_context(c.dec, st->codecpar) < 0) return false;
    c.dec->thread_count = 0;
    c.dec->skip_loop_filter = AVDISCARD_ALL;
    c.dec->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(c.dec, codec, nullptr) < 0) return false;
    c.pkt = av_packet_alloc();
    c.frame = av_frame_alloc();
    if (!c.pkt || !c.frame) return false;

    // A keyframe some way in: 10% of the clip, at most 10 s (title cards and fades are
    // at the start). Clips without a duration start from the top.
    if (c.fmt->duration > 0) {
        const int64_t target = std::min<int64_t>(c.fmt->duration / 10, 10 * (int64_t)AV_TIME_BASE);
        if (target > 0) {
            const int64_t ts = av_rescale_q(target, AVRational{1, AV_TIME_BASE}, st->time_base);
            if (av_seek_frame(c.fmt, stream, ts, AVSEEK_FLAG_BACKWARD) >= 0) avcodec_flush_buffers(c.dec);
        }
    }

    auto overBudget = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count() >= budgetMs;
    };
    // The first frame that is not black, else the last one decoded (up to 12 looked at).
    int frames = 0;
    bool have = false;
    while (!overBudget() && av_read_frame(c.fmt, c.pkt) >= 0) {
        if (c.pkt->stream_index != stream) { av_packet_unref(c.pkt); continue; }
        const int sent = avcodec_send_packet(c.dec, c.pkt);
        av_packet_unref(c.pkt);
        if (sent < 0) continue;
        while (avcodec_receive_frame(c.dec, c.frame) >= 0) {
            ++frames;
            const bool dark = Dark(c.frame);
            if (!dark || frames >= 12) return ToRgba(c.frame, maxEdge, width, height, rgba);
            have = ToRgba(c.frame, maxEdge, width, height, rgba) || have;
        }
    }
    return have;
}

#endif

} // namespace mp
