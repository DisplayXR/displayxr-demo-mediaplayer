// SPDX-License-Identifier: Apache-2.0
#include "pro/LiveAudio.h"

#include "Log.h"

#include <SDL3/SDL.h>

namespace mp::pro {

struct LiveAudioSink::Impl {
    SDL_AudioStream* stream = nullptr;
    ~Impl() {
        if (stream) SDL_DestroyAudioStream(stream);  // closes the device it opened
    }
};

std::shared_ptr<LiveAudioSink> LiveAudioSink::Open(int channels, int pair, bool muted, std::string& err) {
    if (LiveAudioCaptureChannels(pair) == 0 || pair * 2 > channels) {
        err = "invalid audio channel pair";
        return nullptr;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        err = std::string("SDL audio init failed: ") + SDL_GetError();
        return nullptr;
    }
    std::shared_ptr<LiveAudioSink> s(new LiveAudioSink());
    s->impl_ = std::make_unique<Impl>();
    s->channels_ = channels;
    s->pair_ = pair;
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S32;  // the card's samples, as they come (SDL converts to the device)
    spec.channels = 2;
    spec.freq = kLiveAudioRate;
    s->impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s->impl_->stream) {
        err = std::string("no audio output: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    SDL_SetAudioStreamGain(s->impl_->stream, muted ? 0.0f : 1.0f);
    SDL_ResumeAudioStreamDevice(s->impl_->stream);
    return s;
}

LiveAudioSink::~LiveAudioSink() {
    const bool opened = impl_ && impl_->stream;
    impl_.reset();
    if (opened) SDL_QuitSubSystem(SDL_INIT_AUDIO);  // balances Open's init (ref-counted by SDL)
}

void LiveAudioSink::Push(const void* samples, int frames) {
    if (!samples || frames <= 0) {
        ++bad_;
        return;
    }
    ++packets_;
    frames_ += (uint64_t)frames;
    if (stereo_.size() < (size_t)frames * 2) stereo_.resize((size_t)frames * 2);
    if (!ExtractStereoPair(static_cast<const int32_t*>(samples), frames, channels_, pair_, stereo_.data())) return;
    const int bytes = frames * 2 * (int)sizeof(int32_t);
    // The card's clock and the output device's are not the same clock: if the queue grows
    // past the limit, start over from this packet rather than drift behind the picture.
    const int maxBytes = kLiveAudioRate * kLiveAudioMaxQueueMs / 1000 * 2 * (int)sizeof(int32_t);
    if (SDL_GetAudioStreamQueued(impl_->stream) > maxBytes) {
        SDL_ClearAudioStream(impl_->stream);
        ++overflows_;
    }
    SDL_PutAudioStreamData(impl_->stream, stereo_.data(), bytes);
}

void LiveAudioSink::SetMuted(bool m) {
    if (impl_ && impl_->stream) SDL_SetAudioStreamGain(impl_->stream, m ? 0.0f : 1.0f);
}

LiveAudioSink::Stats LiveAudioSink::GetStats() const {
    Stats s;
    s.channels = channels_;
    s.pair = pair_;
    s.packets = packets_.load();
    s.sampleFrames = frames_.load();
    s.overflows = overflows_.load();
    s.badPackets = bad_.load();
    return s;
}

}  // namespace mp::pro
