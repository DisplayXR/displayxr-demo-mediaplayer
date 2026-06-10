// SPDX-License-Identifier: BSL-1.0
#include "media/VideoDecoder.h"

#include "Log.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(MEDIAPLAYER_WITH_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace mp {

#if defined(MEDIAPLAYER_WITH_FFMPEG)

// Passed via AVCodecContext::opaque to the get_format hook. Carries the wanted GPU
// surface format plus write-backs so the hook can record a software fallback for the
// HUD/logs. Defined before Impl so Impl can embed one.
struct HwFormatHook {
    enum AVPixelFormat want = AV_PIX_FMT_NONE;  // the decoder's GPU surface format
    bool* hardware = nullptr;                   // cleared if we fall back to software
    const char** backendName = nullptr;         // retargeted to "software …" on fallback
};

struct VideoDecoder::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;                  // only for the odd-format -> I420 fallback
    AVBufferRef* hwDevice = nullptr;            // hwaccel device (null = software decode)
    AVFrame* swFrame = nullptr;                 // download target for hardware frames
    AVFrame* convFrame = nullptr;               // I420 fallback target (rare formats)
    enum AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;  // the decoder's GPU surface format
    HwFormatHook hook;                          // ctx->opaque for the get_format callback
    int videoStream = -1;
    int swsW = 0, swsH = 0, swsFmt = -1;        // cached sws source params (fallback)

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (swFrame) av_frame_free(&swFrame);
        if (convFrame) av_frame_free(&convFrame);
        if (codec) avcodec_free_context(&codec);
        if (hwDevice) av_buffer_unref(&hwDevice);
        if (fmt) avformat_close_input(&fmt);
    }
};

namespace {

// Per-OS hwaccel preference, tried in order; the first that initializes wins. The
// list is empty on unknown platforms, which simply yields the software path.
const AVHWDeviceType* PreferredHwTypes(size_t& count) {
#if defined(__APPLE__)
    static const AVHWDeviceType t[] = {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#elif defined(_WIN32)
    // D3D11VA covers Intel/AMD/NVIDIA; CUDA is the NVDEC path for NVIDIA-only stacks.
    static const AVHWDeviceType t[] = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA};
#elif defined(__linux__)
    static const AVHWDeviceType t[] = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA};
#else
    static const AVHWDeviceType t[] = {AV_HWDEVICE_TYPE_NONE};  // none usable
#endif
    count = (t[0] == AV_HWDEVICE_TYPE_NONE) ? 0 : sizeof(t) / sizeof(t[0]);
    return t;
}

// get_format callback: keep the GPU surface format when the decoder offers it, so
// frames stay on the device until we explicitly download them. opaque points at the
// Impl's hwPixFmt (set before avcodec_open2), keeping this free of the private type.
enum AVPixelFormat PickHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    auto* hook = static_cast<HwFormatHook*>(ctx->opaque);
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == hook->want) return *p;
    }
    // The GPU surface format isn't on offer — the runtime hwaccel failed to initialise
    // for this stream (commonly: H.264 wider than the GPU's 4096px DXVA decode limit,
    // as with 5120-wide SBS clips). Returning AV_PIX_FMT_NONE would *abort* the decoder
    // (black video); instead fall back to software so playback continues. frame->format
    // then won't match hwPixFmt, so DecodeLoop skips the download and swscale runs on the
    // CPU frame directly. The flag writes race a HUD read but are word-sized + write-once.
    LOG_WARN("VideoDecoder: hw surface unavailable for this stream — decoding in software");
    if (hook->hardware) *hook->hardware = false;
    if (hook->backendName) *hook->backendName = "software (hw unsupported)";
    return avcodec_default_get_format(ctx, fmts);
}

// Find the decoder's GPU pixel format for `type`, if this build/codec supports it.
enum AVPixelFormat HwPixFmtFor(const AVCodec* dec, AVHWDeviceType type) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
        if (!cfg) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == type) {
            return cfg->pix_fmt;
        }
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { Stop(); }

// Attach a hwaccel device to impl_->codec, trying each preferred type. On success
// sets hardware_/hwName_/hwPixFmt and the get_format hook; returns false (software)
// if none initialize (e.g. headless box, codec unsupported by the GPU).
bool VideoDecoder::TryEnableHwAccel(const void* decoder) {
    const AVCodec* dec = static_cast<const AVCodec*>(decoder);
    if (const char* f = std::getenv("MEDIAPLAYER_FORCE_SOFTWARE"); f && *f && *f != '0') {
        LOG_INFO("VideoDecoder: MEDIAPLAYER_FORCE_SOFTWARE set — skipping hwaccel");
        return false;
    }
    size_t n = 0;
    const AVHWDeviceType* types = PreferredHwTypes(n);
    for (size_t i = 0; i < n; ++i) {
        const AVHWDeviceType type = types[i];
        const enum AVPixelFormat pf = HwPixFmtFor(dec, type);
        if (pf == AV_PIX_FMT_NONE) continue;  // codec can't use this device in this build

        AVBufferRef* dev = nullptr;
        if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) {
            LOG_WARN("VideoDecoder: hwaccel '%s' present but device init failed",
                     av_hwdevice_get_type_name(type));
            continue;
        }

        impl_->hwDevice = dev;
        impl_->hwPixFmt = pf;
        impl_->hook = {pf, &hardware_, &hwName_};  // read/written by PickHwFormat
        impl_->codec->hw_device_ctx = av_buffer_ref(dev);
        impl_->codec->opaque = &impl_->hook;
        impl_->codec->get_format = PickHwFormat;
        hwName_ = av_hwdevice_get_type_name(type);
        hardware_ = true;
        return true;
    }
    return false;
}

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
    codecName_ = dec->name;
    AVStream* st = impl_->fmt->streams[impl_->videoStream];

    // Build the codec context; try hardware decode, fall back to software if either
    // the device won't init or the hw-configured decoder won't open.
    impl_->codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(impl_->codec, st->codecpar);
    const bool wantHw = TryEnableHwAccel(dec);
    // Always allow auto multithreading. Hardware decode ignores thread_count; but when a
    // stream is too wide for the GPU (PickHwFormat falls back to software PER FRAME), this
    // is what keeps that fallback multithreaded instead of crawling on one core — the case
    // for 7680-wide SBS clips, where single-threaded software decode misses the budget.
    impl_->codec->thread_count = 0;

    int rc = avcodec_open2(impl_->codec, dec, nullptr);
    if (rc < 0 && wantHw) {
        LOG_WARN("VideoDecoder: hwaccel '%s' failed to open decoder; retrying software",
                 hwName_);
        // Drop the hw context entirely and rebuild a clean software decoder.
        avcodec_free_context(&impl_->codec);
        av_buffer_unref(&impl_->hwDevice);
        impl_->hwPixFmt = AV_PIX_FMT_NONE;
        hardware_ = false;
        hwName_ = "software";
        impl_->codec = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(impl_->codec, st->codecpar);
        impl_->codec->thread_count = 0;
        rc = avcodec_open2(impl_->codec, dec, nullptr);
    }
    if (rc < 0) {
        LOG_ERROR("VideoDecoder: cannot open decoder for '%s'", path.c_str());
        impl_.reset();
        return false;
    }

    width_ = impl_->codec->width;
    height_ = impl_->codec->height;
    frameRate_ = av_q2d(st->avg_frame_rate);
    if (frameRate_ <= 0.0) frameRate_ = av_q2d(st->r_frame_rate);
    durationSec_ = (impl_->fmt->duration > 0)
                       ? (double)impl_->fmt->duration / (double)AV_TIME_BASE
                       : 0.0;
    positionSec_.store(0.0);
    seekRequest_.store(-1.0);
    ended_.store(false);
    open_ = true;
    LOG_INFO("VideoDecoder: '%s' %dx%d codec=%s backend=%s dur=%.1fs", path.c_str(), width_,
             height_, codecName_, hwName_, durationSec_);

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

    // Copy one plane (stride-aware) into a tightly packed buffer.
    auto copyPlane = [](std::vector<uint8_t>& d, const uint8_t* s, int stride, int wBytes, int h) {
        d.resize((size_t)wBytes * h);
        if (stride == wBytes) {
            std::memcpy(d.data(), s, (size_t)wBytes * h);
        } else {
            for (int y = 0; y < h; ++y)
                std::memcpy(d.data() + (size_t)y * wBytes, s + (size_t)y * stride, wBytes);
        }
    };

    // Download a HW frame if needed, then hand the decoder's NATIVE planar YUV to the
    // ring (no RGBA convert, no downscale — the GPU does both, off this thread). Returns
    // PTS in seconds (-1 unknown, -2 = drop/HW fail). I420 (software) and NV12 (hw) are
    // copied directly; any other format (10-bit, 4:2:2/4:4:4) is swscale'd to I420 first.
    auto fillFrame = [&](AVFrame* f) -> double {
        AVFrame* src = f;
        if (impl_->hwPixFmt != AV_PIX_FMT_NONE && f->format == impl_->hwPixFmt) {
            if (!impl_->swFrame) impl_->swFrame = av_frame_alloc();
            av_frame_unref(impl_->swFrame);
            if (av_hwframe_transfer_data(impl_->swFrame, f, 0) < 0) {
                LOG_ERROR("VideoDecoder: hw frame download failed; dropping frame");
                return -2.0;
            }
            impl_->swFrame->pts = f->pts;
            impl_->swFrame->best_effort_timestamp = f->best_effort_timestamp;
            impl_->swFrame->color_range = f->color_range;
            src = impl_->swFrame;
        }

        const AVPixelFormat sf = (AVPixelFormat)src->format;
        AVFrame* yuv = src;
        PixFormat outFmt;
        if (sf == AV_PIX_FMT_YUV420P || sf == AV_PIX_FMT_YUVJ420P) {
            outFmt = PixFormat::I420;
        } else if (sf == AV_PIX_FMT_NV12) {
            outFmt = PixFormat::NV12;
        } else {
            // Rare formats -> I420 (no scale) so the GPU path stays uniform.
            if (!impl_->convFrame) impl_->convFrame = av_frame_alloc();
            if (!impl_->sws || impl_->swsW != src->width || impl_->swsH != src->height ||
                impl_->swsFmt != sf) {
                if (impl_->sws) sws_freeContext(impl_->sws);
                impl_->sws = sws_getContext(src->width, src->height, sf, src->width, src->height,
                                            AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr,
                                            nullptr);
                impl_->swsW = src->width;
                impl_->swsH = src->height;
                impl_->swsFmt = sf;
                av_frame_unref(impl_->convFrame);
                impl_->convFrame->format = AV_PIX_FMT_YUV420P;
                impl_->convFrame->width = src->width;
                impl_->convFrame->height = src->height;
                av_frame_get_buffer(impl_->convFrame, 32);
            }
            sws_scale(impl_->sws, src->data, src->linesize, 0, src->height, impl_->convFrame->data,
                      impl_->convFrame->linesize);
            yuv = impl_->convFrame;
            outFmt = PixFormat::I420;
        }

        FrameRing::Frame& dst = ring_.WriteBuffer();
        dst.width = yuv->width;
        dst.height = yuv->height;
        dst.format = outFmt;
        dst.fullRange = (src->color_range == AVCOL_RANGE_JPEG) || sf == AV_PIX_FMT_YUVJ420P;
        const int cw = (yuv->width + 1) / 2, ch = (yuv->height + 1) / 2;
        copyPlane(dst.plane[0], yuv->data[0], yuv->linesize[0], yuv->width, yuv->height);
        if (outFmt == PixFormat::I420) {
            copyPlane(dst.plane[1], yuv->data[1], yuv->linesize[1], cw, ch);
            copyPlane(dst.plane[2], yuv->data[2], yuv->linesize[2], cw, ch);
        } else {  // NV12: a single interleaved chroma plane, cw*2 bytes per row
            copyPlane(dst.plane[1], yuv->data[1], yuv->linesize[1], cw * 2, ch);
            dst.plane[2].clear();
        }
        const int64_t ticks = (src->best_effort_timestamp != AV_NOPTS_VALUE)
                                  ? src->best_effort_timestamp : src->pts;
        return (ticks != AV_NOPTS_VALUE) ? (double)ticks * timeBase : -1.0;
    };

    while (!stop_.load()) {
        // Seek (serviced even while paused). A decoder needs several packets after a
        // flush to emit a frame, so we DECODE FORWARD from the keyframe to the exact
        // target, then publish that one. This is the standard accurate-seek primitive;
        // responsiveness comes from coalescing (latest-wins) + decoding on this thread.
        //
        // Future (web-smooth scrubbing on long-GOP / 4K, where exact-during-drag can't
        // keep up): go adaptive — show the nearest keyframe (or a pre-generated low-res
        // thumbnail/storyboard track) during the drag, and resolve to the exact frame on
        // release. The keyframe-preview variant lived here briefly; see git history.
        const double sk = seekRequest_.exchange(-1.0);
        if (sk >= 0.0) {
            const bool preview = seekPreview_.load();  // drag -> nearest keyframe (fast)
            const auto seekT0 = clock::now();
            av_seek_frame(impl_->fmt, impl_->videoStream, (int64_t)(sk / timeBase),
                          AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(impl_->codec);
            firstPtsSec = -1.0;

            // Decode forward to the EXACT target frame and display only that one. We do
            // NOT bail to the keyframe or interrupt mid-decode for a newer scrub request:
            // an exact seek is cheap now (only the displayed frame is downloaded), and the
            // atomic seekRequest_ coalesces — each finished seek immediately re-targets the
            // latest cursor position. The net effect is frame-exact previews that track the
            // cursor at decode rate, instead of jumping between keyframes.
            bool reached = false, filled = false;
            double lastPts = sk;
            while (!stop_.load() && !reached) {
                if (av_read_frame(impl_->fmt, pkt) < 0) break;  // EOF before target
                if (pkt->stream_index != impl_->videoStream) { av_packet_unref(pkt); continue; }
                const int sent = avcodec_send_packet(impl_->codec, pkt);
                av_packet_unref(pkt);
                if (sent != 0) continue;
                while (!stop_.load() && avcodec_receive_frame(impl_->codec, frame) == 0) {
                    // Read the PTS off the raw decoded frame — cheap, no GPU download. Only
                    // the frame we actually display is downloaded/converted (fillFrame);
                    // decoding past intermediate frames is fast, downloading them is not.
                    const int64_t ticks = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                              ? frame->best_effort_timestamp : frame->pts;
                    const double pts = (ticks != AV_NOPTS_VALUE) ? (double)ticks * timeBase : -1.0;
                    // Preview: take the first frame (the keyframe) — no forward decode.
                    // Exact: decode forward until we reach the target frame.
                    if (preview || pts < 0.0 || pts + 1e-3 >= sk) {
                        const double fpts = fillFrame(frame);  // copy this one to the ring
                        if (fpts > -2.0) { filled = true; lastPts = (fpts >= 0.0) ? fpts : sk; }
                        reached = true;
                        break;
                    }
                    // earlier than target → discard cheaply (no copy), keep decoding
                }
            }
            // Always publish the frame we landed on — during a drag this is the preview
            // keyframe; when settled it's the exact target. The knob is held steady by
            // the UI until the decoded position catches up (no snap-back on release).
            if (filled) {
                positionSec_.store(lastPts);
                ring_.Publish();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    clock::now() - seekT0).count();
                LOG_DEBUG("VideoDecoder: seek %.2fs -> landed %.2fs in %lldms", sk, lastPts,
                          (long long)ms);
            }
            wallStart = clock::now();
            continue;
        }

        // Pause: hold here without consuming packets (but wake to service a seek), then
        // advance the wall clock by the paused duration so pacing resumes seamlessly.
        if (paused_.load()) {
            const auto pauseBegin = clock::now();
            while (paused_.load() && seekRequest_.load() < 0.0 && !stop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            wallStart += clock::now() - pauseBegin;
            continue;
        }

        int r = av_read_frame(impl_->fmt, pkt);
        if (r < 0) {
            if (loopEnabled_.load()) {
                // EOF (or error) -> loop back to the start.
                av_seek_frame(impl_->fmt, impl_->videoStream, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(impl_->codec);
                firstPtsSec = -1.0;
                wallStart = clock::now();
            } else {
                // Loop off: mark ended and pause, holding the last published frame. A
                // Seek() (e.g. resume-from-end, or slideshow advance) clears `ended_`.
                ended_.store(true);
                paused_.store(true);
            }
            continue;
        }
        if (pkt->stream_index != impl_->videoStream) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(impl_->codec, pkt) == 0) {
            while (!stop_.load() && avcodec_receive_frame(impl_->codec, frame) == 0) {
                const double ptsSec = fillFrame(frame);
                if (ptsSec <= -2.0) continue;  // dropped
                if (ptsSec >= 0.0) {
                    positionSec_.store(ptsSec);
                    const double mc = masterClock_ ? masterClock_() : -1.0;
                    if (mc >= 0.0) {
                        // Audio is the master clock: present when it reaches this PTS.
                        for (;;) {
                            if (stop_.load() || paused_.load() || seekRequest_.load() >= 0.0) break;
                            const double now = masterClock_();
                            if (now < 0.0) break;               // audio stopped -> show now
                            if (now >= ptsSec - 0.005) break;   // audio caught up
                            if (ptsSec - now > 1.0) break;      // far ahead (loop) -> show now
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                    } else {
                        if (firstPtsSec < 0.0) firstPtsSec = ptsSec;
                        auto target = wallStart + std::chrono::duration_cast<clock::duration>(
                                                      std::chrono::duration<double>(ptsSec - firstPtsSec));
                        std::this_thread::sleep_until(target);
                    }
                }
                ring_.Publish();
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

void VideoDecoder::Seek(double seconds, bool preview) {
    if (seconds < 0.0) seconds = 0.0;
    // Leave a little headroom before EOF so the forward-decode always finds a frame at
    // or after the target (otherwise a seek to the very end lands nothing to display).
    if (durationSec_ > 0.0 && seconds > durationSec_ - 0.1) seconds = durationSec_ - 0.1;
    if (seconds < 0.0) seconds = 0.0;
    LOG_DEBUG("VideoDecoder: seek request -> %.2fs (paused=%d)", seconds, (int)paused_.load());
    ended_.store(false);   // a seek leaves the EOF/hold state
    seekPreview_.store(preview);
    seekRequest_.store(seconds);
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
void VideoDecoder::Seek(double, bool) {}
void VideoDecoder::Stop() {}

#endif

} // namespace mp
