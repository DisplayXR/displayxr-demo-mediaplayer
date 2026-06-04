// SPDX-License-Identifier: BSL-1.0
#include "media/VideoDecoder.h"

#include "Log.h"

#include <chrono>
#include <thread>

#if defined(MEDIAPLAYER_WITH_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace mp {

#if defined(MEDIAPLAYER_WITH_FFMPEG)

struct VideoDecoder::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;
    int videoStream = -1;
    int swsW = 0, swsH = 0, swsFmt = -1;  // cached sws source params

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
    }
};

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { Stop(); }

bool VideoDecoder::Open(const std::string& path) {
    impl_ = std::make_unique<Impl>();

    if (avformat_open_input(&impl_->fmt, path.c_str(), nullptr, nullptr) < 0) {
        LOG_ERROR("VideoDecoder: cannot open '%s'", path.c_str());
        impl_.reset();
        return false;
    }
    if (avformat_find_stream_info(impl_->fmt, nullptr) < 0) {
        LOG_ERROR("VideoDecoder: no stream info in '%s'", path.c_str());
        impl_.reset();
        return false;
    }

    const AVCodec* dec = nullptr;
    impl_->videoStream = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (impl_->videoStream < 0 || !dec) {
        LOG_ERROR("VideoDecoder: no video stream in '%s'", path.c_str());
        impl_.reset();
        return false;
    }

    impl_->codec = avcodec_alloc_context3(dec);
    AVStream* st = impl_->fmt->streams[impl_->videoStream];
    avcodec_parameters_to_context(impl_->codec, st->codecpar);
    impl_->codec->thread_count = 0;  // let FFmpeg pick (multithreaded software decode)
    if (avcodec_open2(impl_->codec, dec, nullptr) < 0) {
        LOG_ERROR("VideoDecoder: cannot open decoder for '%s'", path.c_str());
        impl_.reset();
        return false;
    }

    width_ = impl_->codec->width;
    height_ = impl_->codec->height;
    open_ = true;
    LOG_INFO("VideoDecoder: '%s' %dx%d, codec=%s", path.c_str(), width_, height_, dec->name);

    stop_ = false;
    thread_ = std::thread(&VideoDecoder::DecodeLoop, this);
    return true;
}

void VideoDecoder::DecodeLoop() {
    using clock = std::chrono::steady_clock;
    AVStream* st = impl_->fmt->streams[impl_->videoStream];
    const double timeBase = av_q2d(st->time_base);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto wallStart = clock::now();
    double firstPtsSec = -1.0;

    while (!stop_.load()) {
        int r = av_read_frame(impl_->fmt, pkt);
        if (r < 0) {
            // EOF (or error) -> loop back to the start.
            av_seek_frame(impl_->fmt, impl_->videoStream, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(impl_->codec);
            firstPtsSec = -1.0;
            wallStart = clock::now();
            continue;
        }
        if (pkt->stream_index != impl_->videoStream) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(impl_->codec, pkt) == 0) {
            while (!stop_.load() && avcodec_receive_frame(impl_->codec, frame) == 0) {
                // (Re)create the RGBA converter if the source format/size changed.
                if (!impl_->sws || impl_->swsW != frame->width ||
                    impl_->swsH != frame->height || impl_->swsFmt != frame->format) {
                    if (impl_->sws) sws_freeContext(impl_->sws);
                    impl_->sws = sws_getContext(frame->width, frame->height,
                                                (AVPixelFormat)frame->format,
                                                frame->width, frame->height, AV_PIX_FMT_RGBA,
                                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                    impl_->swsW = frame->width;
                    impl_->swsH = frame->height;
                    impl_->swsFmt = frame->format;
                }

                FrameRing::Frame& dst = ring_.WriteBuffer();
                dst.width = frame->width;
                dst.height = frame->height;
                dst.pixels.resize((size_t)frame->width * frame->height * 4);
                uint8_t* dstData[4] = {dst.pixels.data(), nullptr, nullptr, nullptr};
                int dstStride[4] = {frame->width * 4, 0, 0, 0};
                sws_scale(impl_->sws, frame->data, frame->linesize, 0, frame->height,
                          dstData, dstStride);

                // Pace to the presentation clock so playback runs at real speed.
                int64_t ptsTicks = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                       ? frame->best_effort_timestamp : frame->pts;
                if (ptsTicks != AV_NOPTS_VALUE) {
                    double ptsSec = (double)ptsTicks * timeBase;
                    if (firstPtsSec < 0.0) firstPtsSec = ptsSec;
                    auto target = wallStart + std::chrono::duration_cast<clock::duration>(
                                                  std::chrono::duration<double>(ptsSec - firstPtsSec));
                    std::this_thread::sleep_until(target);
                }

                ring_.Publish();
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

void VideoDecoder::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    open_ = false;
    impl_.reset();
}

#else  // !MEDIAPLAYER_WITH_FFMPEG

struct VideoDecoder::Impl {};
VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() {}
bool VideoDecoder::Open(const std::string&) {
    LOG_WARN("VideoDecoder: built without FFmpeg — video unavailable");
    return false;
}
void VideoDecoder::DecodeLoop() {}
void VideoDecoder::Stop() {}

#endif

} // namespace mp
