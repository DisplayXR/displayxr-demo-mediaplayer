// SPDX-License-Identifier: Apache-2.0
#include "pro/LiveAudio.h"

#include "Log.h"
#include "media/AudioMeter.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace mp::pro {

struct LiveAudioSink::Impl {
    SDL_AudioStream* stream = nullptr;
    ~Impl() {
        if (stream) SDL_DestroyAudioStream(stream);  // closes the device it opened
    }
};

std::shared_ptr<LiveAudioSink> LiveAudioSink::Open(int channels, int pair, bool muted, float volume,
                                                   AudioMeter* meter, std::string& err) {
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
    s->meter_ = meter;
    s->muted_ = muted;
    s->volume_ = std::clamp(volume, 0.0f, 1.0f);
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
    s->ApplyGain();
    if (meter) meter->Clear();
    SDL_ResumeAudioStreamDevice(s->impl_->stream);
    return s;
}

LiveAudioSink::~LiveAudioSink() {
    const bool opened = impl_ && impl_->stream;
    impl_.reset();
    if (meter_) meter_->Clear();
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
    const int bytesPerMs = kLiveAudioRate / 1000 * 2 * (int)sizeof(int32_t);
    const int delayMs = delayMs_.load();
    if (delayMs != appliedDelayMs_) {
        // The sync delay changed: restart the queue with that much silence ahead.
        SDL_ClearAudioStream(impl_->stream);
        if (meter_) meter_->Clear();
        if (delayMs > 0) {
            std::vector<int32_t> silence((size_t)delayMs * (size_t)(kLiveAudioRate / 1000) * 2, 0);
            SDL_PutAudioStreamData(impl_->stream, silence.data(), (int)(silence.size() * sizeof(int32_t)));
        }
        appliedDelayMs_ = delayMs;
    }
    const int maxBytes = (kLiveAudioMaxQueueMs + appliedDelayMs_) * bytesPerMs;
    int queued = SDL_GetAudioStreamQueued(impl_->stream);
    if (queued > maxBytes) {
        SDL_ClearAudioStream(impl_->stream);
        ++overflows_;
        queued = 0;
        if (meter_) meter_->Clear();
    }
    // Every captured channel is metered, not only the pair played: the meters show which
    // channels carry sound.
    if (meter_)
        meter_->AddInt32(static_cast<const int32_t*>(samples), frames, channels_,
                         queued / (2 * (int)sizeof(int32_t)));
    SDL_PutAudioStreamData(impl_->stream, stereo_.data(), bytes);
}

void LiveAudioSink::SetMuted(bool m) {
    muted_ = m;
    ApplyGain();
}

void LiveAudioSink::SetVolume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.0f);
    ApplyGain();
}

void LiveAudioSink::ApplyGain() {
    if (impl_ && impl_->stream) SDL_SetAudioStreamGain(impl_->stream, muted_.load() ? 0.0f : volume_.load());
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
