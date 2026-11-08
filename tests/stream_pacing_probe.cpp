// SPDX-License-Identifier: Apache-2.0
//
// stream_pacing_probe — measures how a stream URL reaches the screen (#197). Opens the
// URL through the real VideoDecoder live path and consumes its FrameRing the way the
// renderer does (AcquireLatest once per display refresh, 60 Hz by default), with no
// OpenXR, window or GPU. Prints, for the run:
//   decoded      frames the decoder published (stats.frames)
//   overwritten  publishes the consumer never saw (serial gaps)
//   shown        frames the consumer actually took
//   media/wall   media time advanced per wall second while shown (1.00 = real time)
//   pts step     PTS delta between consecutive SHOWN frames (mean / max, ms)
//   gaps>100ms   refreshes where the screen held one picture longer than 100 ms
//   rung / switches / estimate / buffer   adaptive bitrate (#205); once a second with
//                the 4th argument, which also fixes the quality (-1 = auto)
// Not a ctest: it needs the network (tests/segmented_pacing_test.cpp is the offline
// check). Built with MP_BUILD_TESTS. Usage:
//   stream_pacing_probe <url> [seconds=30] [refresh_hz=60] [quality=-1 | -2 = auto, quiet]
//                       [schedule "t:quality,...", e.g. "20:0,40:-1" = rung 0 at 20 s, auto at 40 s]
#include "media/FrameRing.h"
#include "media/VideoDecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <url> [seconds=30] [refresh_hz=60] [quality]\n", argv[0]);
        return 2;
    }
    const std::string url = argv[1];
    const double seconds = argc > 2 ? std::atof(argv[2]) : 30.0;
    const double hz = argc > 3 ? std::atof(argv[3]) : 60.0;
    const int quality = argc > 4 ? std::atoi(argv[4]) : -2;  // -2: auto, no per-second line
    std::vector<std::pair<double, int>> schedule;  // (seconds after open, quality)
    if (argc > 5) {
        std::string spec = argv[5];
        size_t at = 0;
        while (at < spec.size()) {
            const size_t comma = spec.find(',', at);
            const std::string item = spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            const size_t colon = item.find(':');
            if (colon != std::string::npos)
                schedule.emplace_back(std::atof(item.substr(0, colon).c_str()), std::atoi(item.substr(colon + 1).c_str()));
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
    }
    using clock = std::chrono::steady_clock;

    mp::VideoDecoder dec;
    dec.OpenLive(url);
    if (quality >= -1) dec.SetStreamQuality(quality);
    auto nextReport = clock::now() + std::chrono::seconds(1);
    const auto t0 = clock::now();
    const auto tick = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = t0;

    bool started = false;
    clock::time_point firstShown, lastShown, lastNew;
    double firstPts = -1, lastPts = -1, prevPts = -1;
    int64_t prevSerial = 0, firstSerial = 0;
    uint64_t shown = 0, overwritten = 0, gaps100 = 0, backwards = 0;
    std::vector<double> steps, holds;
    while (std::chrono::duration<double>(clock::now() - t0).count() < seconds + 60.0) {
        next += tick;
        std::this_thread::sleep_until(next);
        const auto now = clock::now();
        if (dec.GetStreamState() == mp::VideoDecoder::StreamState::Failed) {
            std::fprintf(stderr, "stream failed: %s\n", dec.GetStreamStats().lastError.c_str());
            return 1;
        }
        for (auto& sq : schedule) {
            if (sq.first >= 0.0 && std::chrono::duration<double>(now - t0).count() >= sq.first) {
                std::printf("t=%5.1f quality -> %d\n", sq.first, sq.second);
                dec.SetStreamQuality(sq.second);
                sq.first = -1.0;
            }
        }
        if (quality >= -1 && now >= nextReport) {
            nextReport += std::chrono::seconds(1);
            const mp::VideoDecoder::StreamStats s = dec.GetStreamStats();
            std::printf("t=%5.1f rung=%d/%d %dx%d est=%.0f kbit/s buffer=%.1f s switches=%d stalls=%llu\n",
                        std::chrono::duration<double>(now - t0).count(), s.rung, s.rungs, s.width, s.height,
                        s.estKbps, s.bufferSec, s.switches, (unsigned long long)s.stalls);
            std::fflush(stdout);
        }
        const mp::FrameRing::Frame* f = dec.Ring().AcquireLatest();
        if (!f) continue;
        const double pts = dec.PositionSeconds();
        if (!started) {
            started = true;
            firstShown = lastNew = now;
            firstPts = pts;
            firstSerial = f->serial;
            std::fprintf(stderr, "first frame %.0f ms after open, %dx%d\n",
                         std::chrono::duration<double, std::milli>(now - t0).count(), f->width, f->height);
        } else {
            if (f->serial > prevSerial + 1) overwritten += (uint64_t)(f->serial - prevSerial - 1);
            const double hold = std::chrono::duration<double, std::milli>(now - lastNew).count();
            holds.push_back(hold);
            if (hold > 100.0) ++gaps100;
            if (pts >= 0 && prevPts >= 0) {
                if (pts < prevPts) ++backwards;
                else steps.push_back((pts - prevPts) * 1000.0);
            }
        }
        prevSerial = f->serial;
        prevPts = pts;
        lastPts = pts;
        lastNew = lastShown = now;
        ++shown;
        if (std::chrono::duration<double>(now - firstShown).count() >= seconds) break;
    }
    const mp::VideoDecoder::StreamStats ss = dec.GetStreamStats();
    dec.Stop();
    if (!started) {
        std::fprintf(stderr, "no frame\n");
        return 1;
    }
    const double wall = std::chrono::duration<double>(lastShown - firstShown).count();
    double mean = 0, mx = 0, hmean = 0, hmax = 0;
    for (double s : steps) { mean += s; mx = std::max(mx, s); }
    if (!steps.empty()) mean /= steps.size();
    double sd = 0;
    for (double s : steps) sd += (s - mean) * (s - mean);
    if (!steps.empty()) sd = std::sqrt(sd / steps.size());
    for (double h : holds) { hmean += h; hmax = std::max(hmax, h); }
    if (!holds.empty()) hmean /= holds.size();
    std::printf("url=%s\n", url.c_str());
    std::printf("res=%dx%d codec=%s/%s fps_nominal=%.3f\n", ss.width, ss.height, ss.codec.c_str(),
                ss.backend.c_str(), dec.FrameRate());
    std::printf("wall=%.1fs decoded=%llu dropped_stat=%llu overwritten=%llu shown=%llu shown_fps=%.1f\n", wall,
                (unsigned long long)ss.frames, (unsigned long long)ss.dropped,
                (unsigned long long)overwritten, (unsigned long long)shown, wall > 0 ? (shown - 1) / wall : 0.0);
    std::printf("media/wall=%.3f pts_step_ms mean=%.1f sd=%.1f max=%.1f backwards=%llu\n",
                wall > 0 ? (lastPts - firstPts) / wall : 0.0, mean, sd, mx, (unsigned long long)backwards);
    std::printf("hold_ms mean=%.1f max=%.1f gaps>100ms=%llu reconnects=%d\n", hmean, hmax,
                (unsigned long long)gaps100, ss.reconnects);
    std::printf("stalls=%llu rung=%d/%d switches=%d est=%.0f kbit/s\n", (unsigned long long)ss.stalls, ss.rung,
                ss.rungs, ss.switches, ss.estKbps);
    return 0;
}
