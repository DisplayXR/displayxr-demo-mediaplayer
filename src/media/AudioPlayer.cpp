// SPDX-License-Identifier: BSL-1.0
#include "media/AudioPlayer.h"

#include "Log.h"

#include <chrono>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#if defined(MEDIAPLAYER_WITH_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#endif

namespace mp {

#if defined(MEDIAPLAYER_WITH_FFMPEG)

struct AudioPlayer::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    SDL_AudioStream* stream = nullptr;
    int audioStream = -1;
    int outRate = 48000;
    int outChannels = 2;
    double timeBase = 0.0;
    std::atomic<double> firstPts{-1.0};        // PTS of the first sample since open/seek
    std::atomic<long long> pushedSamples{0};   // per-channel sample frames pushed to SDL

    ~Impl() {
        if (stream) SDL_DestroyAudioStream(stream);
        if (swr) swr_free(&swr);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
    }
};

AudioPlayer::AudioPlayer() = default;
AudioPlayer::~AudioPlayer() { Stop(); }

bool AudioPlayer::Open(const std::string& path) {
    impl_ = std::make_unique<Impl>();

    if (avformat_open_input(&impl_->fmt, path.c_str(), nullptr, nullptr) < 0) {
        impl_.reset();
        return false;
    }
    if (avformat_find_stream_info(impl_->fmt, nullptr) < 0) {
        impl_.reset();
        return false;
    }
    const AVCodec* dec = nullptr;
    impl_->audioStream = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (impl_->audioStream < 0 || !dec) {  // no audio stream — silent playback
        impl_.reset();
        return false;
    }
    AVStream* st = impl_->fmt->streams[impl_->audioStream];
    impl_->codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(impl_->codec, st->codecpar);
    impl_->codec->thread_count = 0;
    if (avcodec_open2(impl_->codec, dec, nullptr) < 0) {
        impl_.reset();
        return false;
    }
    impl_->timeBase = av_q2d(st->time_base);
    impl_->outRate = impl_->codec->sample_rate > 0 ? impl_->codec->sample_rate : 48000;
    impl_->outChannels = 2;

    // Resample to interleaved float stereo at the source rate (SDL converts to the
    // device rate). Guard an unset input layout (older streams).
    if (impl_->codec->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&impl_->codec->ch_layout, 2);
    }
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, impl_->outChannels);
    int rc = swr_alloc_set_opts2(&impl_->swr, &outLayout, AV_SAMPLE_FMT_FLT, impl_->outRate,
                                 &impl_->codec->ch_layout, impl_->codec->sample_fmt,
                                 impl_->codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (rc < 0 || swr_init(impl_->swr) < 0) {
        LOG_WARN("AudioPlayer: swr init failed — silent");
        impl_.reset();
        return false;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_WARN("AudioPlayer: SDL audio init failed: %s", SDL_GetError());
        impl_.reset();
        return false;
    }
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_F32;
    spec.channels = impl_->outChannels;
    spec.freq = impl_->outRate;
    impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
                                              nullptr);
    if (!impl_->stream) {
        LOG_WARN("AudioPlayer: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        impl_.reset();
        return false;
    }
    SDL_SetAudioStreamGain(impl_->stream, muted_.load() ? 0.0f : 1.0f);
    SDL_ResumeAudioStreamDevice(impl_->stream);

    open_ = true;
    stop_ = false;
    paused_.store(false);
    seekRequest_.store(-1.0);
    LOG_INFO("AudioPlayer: '%s' %s %dch %dHz", dec->name,
             av_get_sample_fmt_name(impl_->codec->sample_fmt), impl_->codec->ch_layout.nb_channels,
             impl_->codec->sample_rate);
    thread_ = std::thread(&AudioPlayer::DecodeLoop, this);
    return true;
}

void AudioPlayer::DecodeLoop() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const int bytesPerFrame = impl_->outChannels * (int)sizeof(float);
    std::vector<uint8_t> outBuf;

    auto resetClock = [&]() {
        if (impl_->stream) SDL_ClearAudioStream(impl_->stream);
        impl_->firstPts.store(-1.0);
        impl_->pushedSamples.store(0);
    };

    while (!stop_.load()) {
        if (const double sk = seekRequest_.exchange(-1.0); sk >= 0.0) {
            av_seek_frame(impl_->fmt, impl_->audioStream, (int64_t)(sk / impl_->timeBase),
                          AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(impl_->codec);
            resetClock();
        }
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // Stay ~0.3s ahead at most, so seeks/pauses respond promptly and memory stays low.
        if (SDL_GetAudioStreamQueued(impl_->stream) > impl_->outRate * bytesPerFrame * 3 / 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }

        const int r = av_read_frame(impl_->fmt, pkt);
        if (r < 0) {  // EOF
            if (loopEnabled_.load()) {
                av_seek_frame(impl_->fmt, impl_->audioStream, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(impl_->codec);
                resetClock();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));  // idle at end
            }
            continue;
        }
        if (pkt->stream_index != impl_->audioStream) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(impl_->codec, pkt) == 0) {
            while (!stop_.load() && avcodec_receive_frame(impl_->codec, frame) == 0) {
                if (impl_->firstPts.load() < 0.0) {
                    const int64_t t = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                          ? frame->best_effort_timestamp : frame->pts;
                    if (t != AV_NOPTS_VALUE) impl_->firstPts.store((double)t * impl_->timeBase);
                }
                const int maxOut = swr_get_out_samples(impl_->swr, frame->nb_samples);
                if (maxOut <= 0) continue;
                outBuf.resize((size_t)maxOut * bytesPerFrame);
                uint8_t* outPtr = outBuf.data();
                const int got = swr_convert(impl_->swr, &outPtr, maxOut,
                                            (const uint8_t**)frame->extended_data, frame->nb_samples);
                if (got > 0) {
                    SDL_PutAudioStreamData(impl_->stream, outBuf.data(), got * bytesPerFrame);
                    impl_->pushedSamples.fetch_add(got);
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

void AudioPlayer::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    open_ = false;
    impl_.reset();
}

void AudioPlayer::SetPaused(bool p) {
    paused_.store(p);
    if (impl_ && impl_->stream) {
        if (p) SDL_PauseAudioStreamDevice(impl_->stream);
        else SDL_ResumeAudioStreamDevice(impl_->stream);
    }
}

void AudioPlayer::SetMuted(bool m) {
    muted_.store(m);
    if (impl_ && impl_->stream) SDL_SetAudioStreamGain(impl_->stream, m ? 0.0f : 1.0f);
}

void AudioPlayer::Seek(double seconds) {
    seekRequest_.store(seconds < 0.0 ? 0.0 : seconds);
}

double AudioPlayer::ClockSeconds() const {
    if (!open_ || !impl_ || !impl_->stream) return -1.0;
    const double fp = impl_->firstPts.load();
    if (fp < 0.0) return -1.0;  // nothing decoded yet
    const int queuedBytes = SDL_GetAudioStreamQueued(impl_->stream);
    const int bytesPerFrame = impl_->outChannels * (int)sizeof(float);
    const double queued = (queuedBytes > 0 ? queuedBytes : 0) / (double)bytesPerFrame;
    double consumed = (double)impl_->pushedSamples.load() - queued;
    if (consumed < 0.0) consumed = 0.0;
    return fp + consumed / (double)impl_->outRate;
}

#else  // !MEDIAPLAYER_WITH_FFMPEG

struct AudioPlayer::Impl {};
AudioPlayer::AudioPlayer() = default;
AudioPlayer::~AudioPlayer() {}
bool AudioPlayer::Open(const std::string&) { return false; }
void AudioPlayer::Stop() {}
void AudioPlayer::SetPaused(bool) {}
void AudioPlayer::SetMuted(bool) {}
void AudioPlayer::Seek(double) {}
void AudioPlayer::DecodeLoop() {}
double AudioPlayer::ClockSeconds() const { return -1.0; }

#endif

} // namespace mp
