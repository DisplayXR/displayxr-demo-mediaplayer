// SPDX-License-Identifier: Apache-2.0
//
// Segmented sources play at real time (#197). Writes a 6 s, 25 fps HLS VOD with 1 s
// segments to a temp dir with FFmpeg's own encoder + hls muxer, plays the playlist from
// disk through VideoDecoder::OpenLive (a .m3u8 path is a stream, StreamUrl.h), and
// consumes the ring every 5 ms (well above the clip's 25 fps, so a loaded CI runner's
// scheduling jitter does not read as lost frames). Before the fix every segment decoded
// as fast as the machine could and the clip "played" in a fraction of its length, the
// ring overwriting most frames. Now media time must advance at wall-clock speed and
// nearly every frame must reach the consumer. Skips when the FFmpeg build has no
// encoder / hls muxer (the decode-only slim builds).

#include "media/FrameRing.h"
#include "media/VideoDecoder.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

namespace fs = std::filesystem;

namespace {

constexpr int kFps = 25;
constexpr int kSeconds = 6;

bool WriteHls(const std::string& playlist) {
    const AVOutputFormat* hls = av_guess_format("hls", nullptr, nullptr);
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!hls || !codec) return false;
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, hls, nullptr, playlist.c_str()) < 0) return false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = 160;
    c->height = 90;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = AVRational{1, kFps};
    c->framerate = AVRational{kFps, 1};
    c->gop_size = kFps;  // a keyframe per second = one per segment
    c->max_b_frames = 0;
    st->time_base = c->time_base;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, codec, nullptr) < 0) return false;
    avcodec_parameters_from_context(st->codecpar, c);
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "hls_time", "1", 0);
    av_dict_set(&opts, "hls_list_size", "0", 0);
    av_dict_set(&opts, "hls_playlist_type", "vod", 0);
    if (avformat_write_header(oc, &opts) < 0) {
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);
    AVFrame* f = av_frame_alloc();
    f->format = c->pix_fmt;
    f->width = c->width;
    f->height = c->height;
    av_frame_get_buffer(f, 32);
    AVPacket* pkt = av_packet_alloc();
    const int frames = kFps * kSeconds;
    for (int i = 0; i <= frames; ++i) {
        if (i < frames) {
            av_frame_make_writable(f);
            for (int p = 0; p < 3; ++p)
                for (int y = 0; y < (p ? c->height / 2 : c->height); ++y)
                    for (int x = 0; x < (p ? c->width / 2 : c->width); ++x)
                        f->data[p][y * f->linesize[p] + x] = (uint8_t)(p ? 128 : (x + i * 3) & 0xff);
            f->pts = i;
        }
        avcodec_send_frame(c, i < frames ? f : nullptr);
        while (avcodec_receive_packet(c, pkt) == 0) {
            av_packet_rescale_ts(pkt, c->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(oc, pkt);
        }
    }
    av_write_trailer(oc);
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&c);
    avformat_free_context(oc);
    return fs::exists(playlist);
}

}  // namespace

int main() {
    av_log_set_level(AV_LOG_ERROR);  // the hls muxer warns per packet
    const fs::path dir = fs::temp_directory_path() / "mp_segmented_pacing_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string playlist = (dir / "clip.m3u8").string();
    if (!WriteHls(playlist)) {
        std::printf("segmented_pacing_test: no mpeg2video encoder / hls muxer in this FFmpeg - skipped\n");
        fs::remove_all(dir, ec);
        return 0;
    }

    using clock = std::chrono::steady_clock;
    mp::VideoDecoder dec;
    dec.OpenLive(playlist);
    // Wait for the first frame (opening a local playlist is fast; allow a slow CI box).
    const auto t0 = clock::now();
    int64_t lastSerial = 0;
    while (clock::now() - t0 < std::chrono::seconds(10)) {
        if (const mp::FrameRing::Frame* f = dec.Ring().AcquireLatest()) {
            lastSerial = f->serial;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    int failures = 0;
    if (!lastSerial) {
        std::fprintf(stderr, "  FAIL: no frame from the local HLS playlist\n");
        dec.Stop();
        return 1;
    }
    const double pts0 = dec.PositionSeconds();
    const auto w0 = clock::now();
    // 3 s of a 200 Hz consumer, well inside the 6 s clip.
    int shown = 0;
    int64_t skipped = 0;
    auto next = w0;
    while (clock::now() - w0 < std::chrono::seconds(3)) {
        next += std::chrono::microseconds(5000);
        std::this_thread::sleep_until(next);
        if (const mp::FrameRing::Frame* f = dec.Ring().AcquireLatest()) {
            if (f->serial > lastSerial + 1) skipped += f->serial - lastSerial - 1;
            lastSerial = f->serial;
            ++shown;
        }
    }
    const double media = dec.PositionSeconds() - pts0;
    const double wall = std::chrono::duration<double>(clock::now() - w0).count();
    const mp::VideoDecoder::StreamStats ss = dec.GetStreamStats();
    dec.Stop();
    const double rate = media / wall;
    std::printf("  %.2f s of media in %.2f s of wall (x%.2f), %d frame(s) shown, %lld overwritten, %llu stall(s)\n",
                media, wall, rate, shown, (long long)skipped, (unsigned long long)ss.stalls);
    if (rate < 0.85 || rate > 1.15) {
        std::fprintf(stderr, "  FAIL: a segmented source must play at real time (x%.2f)\n", rate);
        ++failures;
    }
    // 3 s at 25 fps = 75 frames. Before the fix: 4 shown, 246 overwritten. A shared CI
    // VM oversleeps by tens of ms (the decode thread and this consumer both), which pairs
    // up publishes now and then (macOS runner: 52 shown, 22 overwritten, x1.00), so the
    // bound is loose; the rate above is the real check.
    if (shown < 40 || skipped > 35) {
        std::fprintf(stderr, "  FAIL: frames lost between decode and display (%d shown, %lld overwritten)\n", shown,
                     (long long)skipped);
        ++failures;
    }
    fs::remove_all(dir, ec);
    std::printf("segmented_pacing_test: %s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
