// SPDX-License-Identifier: Apache-2.0
//
// The decoder drains at end of file: a clip shorter than a frame-threaded decoder's
// thread count used to show nothing at all, and every clip lost its last few frames.
// Writes a 5-frame ProRes clip with FFmpeg's encoder, plays it through VideoDecoder and
// checks the first frame arrives and the last one is reached.

#include "media/FrameRing.h"
#include "media/VideoDecoder.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace fs = std::filesystem;

namespace {

bool WriteProRes(const std::string& path, int w, int h, int frames) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mov", path.c_str()) < 0) return false;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PRORES);
    if (!codec) return false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->pix_fmt = AV_PIX_FMT_YUV422P10LE;
    c->time_base = AVRational{1, 24};
    st->time_base = c->time_base;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, codec, nullptr) < 0) return false;
    avcodec_parameters_from_context(st->codecpar, c);
    if (avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(oc, nullptr) < 0) return false;
    AVFrame* f = av_frame_alloc();
    f->format = c->pix_fmt;
    f->width = w;
    f->height = h;
    av_frame_get_buffer(f, 32);
    AVPacket* pkt = av_packet_alloc();
    for (int i = 0; i <= frames; ++i) {
        if (i < frames) {
            av_frame_make_writable(f);
            for (int p = 0; p < 3; ++p)
                for (int y = 0; y < h; ++y) {
                    uint16_t* r = (uint16_t*)(f->data[p] + (size_t)y * f->linesize[p]);
                    for (int x = 0; x < (p ? w / 2 : w); ++x) r[x] = p ? 512 : (uint16_t)(100 + i * 100);
                }
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
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    return true;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "mp_short_clip_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = (dir / "five_frames.mov").string();
    if (!WriteProRes(path, 128, 64, 5)) {
        std::printf("short_clip_test: no ProRes encoder in this FFmpeg - skipped\n");
        return 0;
    }
    mp::VideoDecoder dec;
    int failures = 0;
    if (!dec.Open(path)) {
        std::fprintf(stderr, "  FAIL: cannot open the clip\n");
        return 1;
    }
    int frames = 0;
    double lastPos = -1.0;
    for (int i = 0; i < 300 && !(dec.Ended() && frames > 0 && lastPos > 0.15); ++i) {
        if (dec.Ring().AcquireLatest()) ++frames;
        lastPos = dec.PositionSeconds();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    dec.Stop();
    std::printf("  5-frame clip: %d frame(s) shown, last position %.3f s (last frame at %.3f s)\n", frames, lastPos,
                4.0 / 24.0);
    if (frames == 0) {
        std::fprintf(stderr, "  FAIL: a clip shorter than the decoder's threads shows nothing\n");
        ++failures;
    }
    if (lastPos < 4.0 / 24.0 - 0.01) {
        std::fprintf(stderr, "  FAIL: the last frame is never reached\n");
        ++failures;
    }
    fs::remove_all(dir, ec);
    std::printf("short_clip_test: %s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
