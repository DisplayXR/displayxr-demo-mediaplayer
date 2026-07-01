// SPDX-License-Identifier: Apache-2.0
//
// AudioPlayer — decode a file's audio stream on its own thread (FFmpeg + swresample)
// and play it through SDL3. Independent of VideoDecoder (its own AVFormatContext over
// the same file), but it exposes a playback clock so the video can sync to it (audio is
// the A/V master). Supports pause, seek, loop, and mute.
//
// FFmpeg/SDL types are hidden behind a pimpl. Built without FFmpeg, Open() returns false
// (the player just runs silent).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace mp {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Open the file's audio stream and start playback. Returns false if there is no
    // audio stream, FFmpeg is absent, or the audio device can't open (-> silent).
    bool Open(const std::string& path);
    void Stop();                       // join the thread + release the device
    bool HasAudio() const { return open_; }

    void SetPaused(bool p);            // pauses/resumes the device + decode
    void SetMuted(bool m);             // gain 0 (keeps playing, just silent)
    bool Muted() const { return muted_.load(); }
    void SetLoop(bool v) { loopEnabled_.store(v); }
    void Seek(double seconds);

    // Position currently being played, in seconds — the A/V master clock. Returns -1
    // when unavailable (no audio / nothing decoded yet), so the video falls back to its
    // own wall clock.
    double ClockSeconds() const;

private:
    void DecodeLoop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> loopEnabled_{false};
    std::atomic<double> seekRequest_{-1.0};
    bool open_ = false;
};

} // namespace mp
