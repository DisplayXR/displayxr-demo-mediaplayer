// SPDX-License-Identifier: Apache-2.0
//
// Pro review formats through the real VideoDecoder: writes tiny EXR and DPX sequences
// and a 10-bit ProRes clip with FFmpeg's own encoders into a temp folder, plays each
// back and checks what reaches the frame ring - the sequence opens as one clip, EXR is
// encoded for display (not dark) at 16 bits, 10-bit sources keep 16 bits (P016), and the
// stream's colour matrix / transfer reach the frame's tags.

#include "media/FrameRing.h"
#include "media/VideoDecoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// Encode `frames` frames of a flat picture to `path` (an image2 pattern or a .mov).
// fill(frame) sets the pixel values.
bool Write(const std::string& path, const char* muxer, AVCodecID codecId, AVPixelFormat pf, int w, int h,
           int frames, int startNumber, void (*fill)(AVFrame*), AVColorSpace cs = AVCOL_SPC_UNSPECIFIED,
           AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, muxer, path.c_str()) < 0) return false;
    const AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) return false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->pix_fmt = pf;
    c->time_base = AVRational{1, 24};
    // The mov muxer writes its 'colr' atom only when all three are known, so primaries
    // follow the matrix.
    const AVColorPrimaries prim = cs == AVCOL_SPC_BT2020_NCL ? AVCOL_PRI_BT2020
                                  : cs == AVCOL_SPC_BT709    ? AVCOL_PRI_BT709
                                                             : AVCOL_PRI_UNSPECIFIED;
    c->colorspace = cs;
    c->color_trc = trc;
    c->color_primaries = prim;
    st->time_base = c->time_base;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, codec, nullptr) < 0) return false;
    avcodec_parameters_from_context(st->codecpar, c);
    if (startNumber >= 0) av_opt_set_int(oc->priv_data, "start_number", startNumber, 0);
    if (!(oc->oformat->flags & AVFMT_NOFILE) && avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return false;
    if (avformat_write_header(oc, nullptr) < 0) return false;
    AVFrame* f = av_frame_alloc();
    f->format = pf;
    f->width = w;
    f->height = h;
    f->colorspace = cs;
    f->color_trc = trc;
    f->color_primaries = prim;
    av_frame_get_buffer(f, 32);
    AVPacket* pkt = av_packet_alloc();
    for (int i = 0; i <= frames; ++i) {
        if (i < frames) {
            av_frame_make_writable(f);
            fill(f);
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
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
    avformat_free_context(oc);
    return true;
}

void FillLinearGrey(AVFrame* f) {  // gbrpf32le: scene-linear 0.18 (18% grey)
    for (int p = 0; p < 3; ++p)
        for (int y = 0; y < f->height; ++y) {
            float* row = (float*)(f->data[p] + (size_t)y * f->linesize[p]);
            for (int x = 0; x < f->width; ++x) row[x] = 0.18f;
        }
}

void FillRgb10(AVFrame* f) {  // gbrp10le: code value 512 (mid) in every channel
    for (int p = 0; p < 3; ++p)
        for (int y = 0; y < f->height; ++y) {
            uint16_t* row = (uint16_t*)(f->data[p] + (size_t)y * f->linesize[p]);
            for (int x = 0; x < f->width; ++x) row[x] = 512;
        }
}

void FillYuv10(AVFrame* f) {  // yuv422p10le: Y 600, chroma neutral 512
    for (int y = 0; y < f->height; ++y) {
        uint16_t* r = (uint16_t*)(f->data[0] + (size_t)y * f->linesize[0]);
        for (int x = 0; x < f->width; ++x) r[x] = 600;
        for (int p = 1; p < 3; ++p) {
            uint16_t* c = (uint16_t*)(f->data[p] + (size_t)y * f->linesize[p]);
            for (int x = 0; x < f->width / 2; ++x) c[x] = 512;
        }
    }
}

// Open `path`, wait for the first frame, return a copy of it.
bool FirstFrame(const std::string& path, mp::FrameRing::Frame& out, double& duration, bool& sequence) {
    mp::VideoDecoder dec;
    dec.SetSequenceFps(24.0);
    if (!dec.Open(path)) return false;
    duration = dec.DurationSeconds();
    sequence = dec.IsSequence();
    for (int i = 0; i < 200; ++i) {
        if (const mp::FrameRing::Frame* f = dec.Ring().AcquireLatest()) {
            out.width = f->width;
            out.height = f->height;
            out.format = f->format;
            out.fullRange = f->fullRange;
            out.matrix = f->matrix;
            out.transfer = f->transfer;
            out.plane[0] = f->plane[0];
            out.plane[1] = f->plane[1];
            dec.Stop();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("  no frame from %s: position %.3f s, ended %d, paused %d\n", path.c_str(), dec.PositionSeconds(),
                (int)dec.Ended(), (int)dec.Paused());
    dec.Stop();
    return false;
}

}  // namespace

int main() {
    mp::VideoDecoder::SetProFormats(true);  // Media Player Pro's review formats
    const fs::path dir = fs::temp_directory_path() / "mp_decode_formats_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "exr");
    fs::create_directories(dir / "dpx");
    const int W = 128, H = 64;

    // --- EXR sequence: one clip, encoded for display ------------------------------------
    CHECK(Write((dir / "exr" / "plate.%04d.exr").string(), "image2", AV_CODEC_ID_EXR, AV_PIX_FMT_GBRPF32LE, W, H, 5,
                1001, FillLinearGrey),
          "write EXR sequence");
    mp::FrameRing::Frame f;
    double dur = 0;
    bool seq = false;
    CHECK(FirstFrame((dir / "exr" / "plate.1003.exr").string(), f, dur, seq), "EXR sequence decodes");
    CHECK(seq && std::fabs(dur - 5.0 / 24.0) < 0.02, "5 frames at 24 fps = one clip");
    CHECK(f.format == mp::PixFormat::P016 && f.width == W, "EXR -> P016 (16-bit) at full size");
    CHECK(f.matrix == mp::ColorMatrix::BT709 && !f.fullRange, "EXR encoded as BT.709 limited, and tagged so");
    if (f.format == mp::PixFormat::P016 && f.plane[0].size() >= (size_t)W * H * 2 &&
        f.plane[1].size() >= (size_t)W * (H / 2) * 2) {
        // linear 0.18 -> sRGB 0.46135 -> 10-bit limited Y = 64 + 0.46135*876 = 468.14, kept
        // with its fraction at 16 bits (x64 = 29961); grey chroma is neutral 512 (32768).
        const uint16_t y = reinterpret_cast<const uint16_t*>(f.plane[0].data())[(size_t)(H / 2) * W + W / 2];
        const uint16_t* uv = reinterpret_cast<const uint16_t*>(f.plane[1].data()) + (size_t)(H / 4) * W + W / 2;
        std::printf("  EXR 18%% grey -> P016 Y %u (%.2f in 10-bit), Cb %.2f Cr %.2f\n", y, y / 64.0, uv[0] / 64.0,
                    uv[1] / 64.0);
        CHECK(std::fabs(y / 64.0 - 468.14) < 0.25, "scene-linear 18% grey encoded for display at 16 bits (Y10 ~468.1)");
        CHECK(std::fabs(uv[0] / 64.0 - 512.0) < 0.25 && std::fabs(uv[1] / 64.0 - 512.0) < 0.25,
              "EXR grey chroma neutral");
    }

    // --- DPX sequence (10-bit RGB) -------------------------------------------------------
    CHECK(Write((dir / "dpx" / "scan.%04d.dpx").string(), "image2", AV_CODEC_ID_DPX, AV_PIX_FMT_GBRP10LE, W, H, 3, 1,
                FillRgb10),
          "write DPX sequence");
    CHECK(FirstFrame((dir / "dpx" / "scan.0001.dpx").string(), f, dur, seq), "DPX sequence decodes");
    std::printf("  DPX format %d (P016 = %d)\n", (int)f.format, (int)mp::PixFormat::P016);
    CHECK(f.format == mp::PixFormat::P016, "10-bit DPX keeps 16 bits (P016)");
    if (f.format == mp::PixFormat::P016 && f.plane[0].size() >= (size_t)W * H * 2) {
        // RGB code 512 of 1023 (grey) -> BT.709 limited luma 64 + 0.5005*876 = 502.4 (10-bit)
        const uint16_t y = reinterpret_cast<const uint16_t*>(f.plane[0].data())[(size_t)(H / 2) * W + W / 2];
        const uint16_t u = reinterpret_cast<const uint16_t*>(f.plane[1].data())[(size_t)(H / 4) * W + W / 2];
        std::printf("  DPX grey 512 -> Y %.1f, Cb %.1f (10-bit)\n", y / 64.0, u / 64.0);
        CHECK(std::fabs(y / 64.0 - 502.4) < 4.0, "DPX grey keeps its level");
        CHECK(std::fabs(u / 64.0 - 512.0) < 4.0, "DPX grey has neutral chroma");
    }

    // --- ProRes 10-bit 4:2:2 -------------------------------------------------------------
    CHECK(Write((dir / "clip.mov").string(), "mov", AV_CODEC_ID_PRORES, AV_PIX_FMT_YUV422P10LE, W, H, 4, -1,
                FillYuv10),
          "write ProRes");
    CHECK(FirstFrame((dir / "clip.mov").string(), f, dur, seq), "ProRes decodes");
    CHECK(f.format == mp::PixFormat::P016 && !seq, "ProRes 10-bit -> P016, not a sequence");
    if (f.format == mp::PixFormat::P016 && f.plane[0].size() >= (size_t)W * H * 2) {
        const uint16_t y = reinterpret_cast<const uint16_t*>(f.plane[0].data())[(size_t)(H / 2) * W + W / 2];
        std::printf("  ProRes Y10 600 -> P016 %u (%.1f in 10-bit)\n", y, y / 64.0);
        CHECK(std::fabs(y / 64.0 - 600.0) < 3.0, "10-bit luma value kept");
    }

    CHECK(f.matrix == mp::ColorMatrix::BT601 && f.transfer == mp::ColorTransfer::SDR,
          "untagged SD (64-line) clip -> BT.601 by the SD rule, SDR");

    // --- Colour tags: the stream's matrix + transfer reach the frame -------------------
    CHECK(Write((dir / "hlg2020.mov").string(), "mov", AV_CODEC_ID_PRORES, AV_PIX_FMT_YUV422P10LE, W, H, 2, -1,
                FillYuv10, AVCOL_SPC_BT2020_NCL, AVCOL_TRC_ARIB_STD_B67),
          "write BT.2020 HLG ProRes");
    CHECK(FirstFrame((dir / "hlg2020.mov").string(), f, dur, seq), "BT.2020 HLG ProRes decodes");
    std::printf("  tagged clip -> %s / %s\n", mp::ColorMatrixName(f.matrix), mp::ColorTransferName(f.transfer));
    CHECK(f.matrix == mp::ColorMatrix::BT2020, "BT.2020 NCL stream tagged BT.2020");
    CHECK(f.transfer == mp::ColorTransfer::HLG, "HLG transfer carried as a tag");
    CHECK(Write((dir / "sd709.mov").string(), "mov", AV_CODEC_ID_PRORES, AV_PIX_FMT_YUV422P10LE, W, H, 2, -1,
                FillYuv10, AVCOL_SPC_BT709, AVCOL_TRC_BT709),
          "write BT.709 ProRes");
    CHECK(FirstFrame((dir / "sd709.mov").string(), f, dur, seq), "BT.709 ProRes decodes");
    CHECK(f.matrix == mp::ColorMatrix::BT709, "an explicit BT.709 tag wins over the SD rule");

    if (!std::getenv("MP_KEEP_TEST_FILES")) fs::remove_all(dir, ec);
    if (g_failures) {
        std::fprintf(stderr, "decode_formats_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("decode_formats_test: all passed\n");
    return 0;
}
