// SPDX-License-Identifier: BSL-1.0
#include "media/VideoDecoder.h"

#include "Log.h"

#include <chrono>
#include <cstdlib>
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
    SwsContext* sws = nullptr;
    AVBufferRef* hwDevice = nullptr;            // hwaccel device (null = software decode)
    AVFrame* swFrame = nullptr;                 // download target for hardware frames
    enum AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;  // the decoder's GPU surface format
    HwFormatHook hook;                          // ctx->opaque for the get_format callback
    int videoStream = -1;
    int swsW = 0, swsH = 0, swsFmt = -1;        // cached sws source params

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (swFrame) av_frame_free(&swFrame);
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
    if (!wantHw) impl_->codec->thread_count = 0;  // multithreaded software decode

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
    durationSec_ = (impl_->fmt->duration > 0)
                       ? (double)impl_->fmt->duration / (double)AV_TIME_BASE
                       : 0.0;
    positionSec_.store(0.0);
    seekRequest_.store(-1.0);
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

    while (!stop_.load()) {
        // Seek (serviced even while paused): jump to the keyframe at/before the target,
        // flush the decoder, and re-anchor the presentation clock. After a seek we fall
        // through once to decode + publish a frame at the new position, then re-pause.
        const double sk = seekRequest_.exchange(-1.0);
        bool justSought = false;
        if (sk >= 0.0) {
            const int64_t ts = (int64_t)(sk / timeBase);
            av_seek_frame(impl_->fmt, impl_->videoStream, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(impl_->codec);
            firstPtsSec = -1.0;
            wallStart = clock::now();
            justSought = true;
        }

        // Pause: hold here without consuming packets (but wake to service a seek), then
        // advance the wall clock by the paused duration so pacing resumes seamlessly.
        if (paused_.load() && !justSought) {
            const auto pauseBegin = clock::now();
            while (paused_.load() && seekRequest_.load() < 0.0 && !stop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            wallStart += clock::now() - pauseBegin;
            continue;
        }

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
                // Hardware decode leaves the frame on the GPU. Download it to system
                // memory (the universal interop path); zero-copy GPU→Vulkan is later.
                // swscale then runs on the downloaded surface (typically NV12/P010).
                AVFrame* src = frame;
                if (impl_->hwPixFmt != AV_PIX_FMT_NONE &&
                    frame->format == impl_->hwPixFmt) {
                    if (!impl_->swFrame) impl_->swFrame = av_frame_alloc();
                    av_frame_unref(impl_->swFrame);
                    if (av_hwframe_transfer_data(impl_->swFrame, frame, 0) < 0) {
                        LOG_ERROR("VideoDecoder: hw frame download failed; dropping frame");
                        continue;
                    }
                    impl_->swFrame->pts = frame->pts;
                    impl_->swFrame->best_effort_timestamp = frame->best_effort_timestamp;
                    src = impl_->swFrame;
                }

                // (Re)create the RGBA converter if the source format/size changed.
                if (!impl_->sws || impl_->swsW != src->width ||
                    impl_->swsH != src->height || impl_->swsFmt != src->format) {
                    if (impl_->sws) sws_freeContext(impl_->sws);
                    impl_->sws = sws_getContext(src->width, src->height,
                                                (AVPixelFormat)src->format,
                                                src->width, src->height, AV_PIX_FMT_RGBA,
                                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                    impl_->swsW = src->width;
                    impl_->swsH = src->height;
                    impl_->swsFmt = src->format;
                }

                FrameRing::Frame& dst = ring_.WriteBuffer();
                dst.width = src->width;
                dst.height = src->height;
                dst.pixels.resize((size_t)src->width * src->height * 4);
                uint8_t* dstData[4] = {dst.pixels.data(), nullptr, nullptr, nullptr};
                int dstStride[4] = {src->width * 4, 0, 0, 0};
                sws_scale(impl_->sws, src->data, src->linesize, 0, src->height,
                          dstData, dstStride);

                // Pace to the presentation clock so playback runs at real speed; track
                // the current position for the scrubber. A post-seek frame served while
                // paused isn't paced (firstPtsSec just reset → target is now anyway).
                int64_t ptsTicks = (src->best_effort_timestamp != AV_NOPTS_VALUE)
                                       ? src->best_effort_timestamp : src->pts;
                if (ptsTicks != AV_NOPTS_VALUE) {
                    double ptsSec = (double)ptsTicks * timeBase;
                    positionSec_.store(ptsSec);
                    if (firstPtsSec < 0.0) firstPtsSec = ptsSec;
                    if (!paused_.load()) {
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

void VideoDecoder::Seek(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    if (durationSec_ > 0.0 && seconds > durationSec_) seconds = durationSec_;
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
void VideoDecoder::Seek(double) {}
void VideoDecoder::Stop() {}

#endif

} // namespace mp
