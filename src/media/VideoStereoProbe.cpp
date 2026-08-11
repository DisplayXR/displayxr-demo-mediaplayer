// SPDX-License-Identifier: Apache-2.0
#include "VideoStereoProbe.h"

#include "Log.h"
#include "StereoDetect.h"

#include <chrono>

#if defined(MEDIAPLAYER_WITH_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/stereo3d.h>
}
#endif

namespace mp {

#if !defined(MEDIAPLAYER_WITH_FFMPEG)

VideoStereoProbe::Result VideoStereoProbe::Run(const std::string&, bool, int) {
    return VideoStereoProbe::Result{};
}

#else

namespace {

constexpr int kMaxFrames = 40;   // ~1.3 s at 30 fps — enough to clear an opening fade
constexpr int kDarkMean = 12;    // frames dimmer than this are skipped outright

// Read AVStereo3D off a decoded frame.
//
// This is the ONLY metadata route, on purpose. av_frame_get_side_data() has been stable
// from libavutil 55 through 8.x and needs no version guard, and it sees strictly more
// than the stream-level API: MKV StereoMode and MP4 st3d/sv3d both propagate here, AND
// the H.264 frame_packing_arrangement SEI (plus the HEVC equivalent) is exported by the
// decoders themselves, which the stream-level API never sees.
//
// DO NOT "optimise" this into av_stream_get_side_data() or codecpar->coded_side_data.
// The former is deprecated at 6.1 and REMOVED at 8.0; the latter does not exist before
// 6.1; and the size out-param changed int*->size_t* at lavf 59. Guarding that correctly
// needs AV_VERSION_INT(60,29,100) precision, not this repo's major-only idiom — 6.0 and
// 6.1 are both major 60 — and CI still builds against ffmpeg 4.4 on Ubuntu 22.04.
bool ReadStereo3D(const AVFrame* f, StereoLayout& layout, bool& invert, const char*& why) {
    const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_STEREO3D);
    if (!sd || sd->size < (int)sizeof(AVStereo3D)) return false;
    const AVStereo3D* s3 = reinterpret_cast<const AVStereo3D*>(sd->data);

    invert = (s3->flags & AV_STEREO3D_FLAG_INVERT) != 0;
    layout = StereoLayout::Mono;

    switch (s3->type) {
        case AV_STEREO3D_SIDEBYSIDE:
        case AV_STEREO3D_SIDEBYSIDE_QUINCUNX:
            if (s3->view != AV_STEREO3D_VIEW_PACKED) {
                // A single view of a multi-view stream: we know it is stereo, but this
                // frame holds one eye and we have no way to present that.
                why = "stereo3d: single-view stream (not packed)";
                return true;
            }
            // NOTE: AVStereo3D says side-by-side but is SILENT about full vs half — the
            // caller resolves that from the frame aspect.
            layout = StereoLayout::SbsFull;
            why = "stereo3d: side-by-side";
            return true;
        case AV_STEREO3D_2D:
            why = "stereo3d: explicitly 2D";
            return true;
        default:
            // Top/bottom, columns, lines, checkerboard: real stereo we cannot render.
            // Report the metadata anyway so we stop guessing and show a sane 2D image.
            why = "stereo3d: unsupported packing";
            return true;
    }
}

// Analyse a decoded frame's luma plane. Handles >8-bit planar formats by sampling the
// high byte; feeding a 16-bit plane through the 8-bit path yields noise that silently
// abstains, which is worse than a wrong answer because it looks like it worked.
StereoDetectResult AnalyzeFrame(const AVFrame* f) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)f->format);
    if (!d || (d->flags & AV_PIX_FMT_FLAG_PAL) || !f->data[0] || f->linesize[0] <= 0) {
        StereoDetectResult r;
        r.abstained = true;
        r.reason = "unsupported pixel format";
        return r;
    }
    const int depth = d->comp[0].depth;
    if (depth <= 8) {
        return StereoDetect::AnalyzeLuma(f->data[0], f->width, f->height, f->linesize[0], 1);
    }
    // 16-bit container: step 2 bytes per sample, and start at the MSB for little-endian.
    const bool be = (d->flags & AV_PIX_FMT_FLAG_BE) != 0;
    const uint8_t* p = f->data[0] + (be ? 0 : 1);
    return StereoDetect::AnalyzeLuma(p, f->width, f->height, f->linesize[0], 2);
}

// Cheap "is this frame basically black" test, on a coarse sample of the luma plane.
bool FrameIsDark(const AVFrame* f) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)f->format);
    if (!d || !f->data[0] || f->linesize[0] <= 0) return false;
    const int step = d->comp[0].depth <= 8 ? 1 : 2;
    const bool be = (d->flags & AV_PIX_FMT_FLAG_BE) != 0;
    const uint8_t* base = f->data[0] + ((step == 2 && !be) ? 1 : 0);
    const int sx = f->width > 64 ? f->width / 64 : 1;
    const int sy = f->height > 64 ? f->height / 64 : 1;
    int64_t acc = 0, n = 0;
    for (int y = 0; y < f->height; y += sy)
        for (int x = 0; x < f->width; x += sx) {
            acc += base[(ptrdiff_t)y * f->linesize[0] + (ptrdiff_t)x * step];
            ++n;
        }
    return n > 0 && (acc / n) < kDarkMean;
}

struct Ctx {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    ~Ctx() {
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
    }
};

} // namespace

VideoStereoProbe::Result VideoStereoProbe::Run(const std::string& path, bool wantContent,
                                               int budgetMs) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    Result out;
    Ctx c;

    if (avformat_open_input(&c.fmt, path.c_str(), nullptr, nullptr) < 0) return out;
    if (avformat_find_stream_info(c.fmt, nullptr) < 0) return out;

#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVCodec* codec = nullptr;
#else
    AVCodec* codec = nullptr;  // ffmpeg 4.x (Ubuntu 22.04): out-param not yet const
#endif
    const int stream = av_find_best_stream(c.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) return out;

    AVStream* st = c.fmt->streams[stream];
    c.dec = avcodec_alloc_context3(codec);
    if (!c.dec) return out;
    if (avcodec_parameters_to_context(c.dec, st->codecpar) < 0) return out;

    // Software only, and as cheap as the decoder will let us be. No hwaccel: a hardware
    // path would cost a device init plus a GPU->CPU download per frame, for pixels we
    // throw away immediately.
    c.dec->thread_count = 0;
    c.dec->skip_loop_filter = AVDISCARD_ALL;
    c.dec->skip_frame = AVDISCARD_NONREF;
    c.dec->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(c.dec, codec, nullptr) < 0) return out;

    out.ok = true;
    out.width = c.dec->width;
    out.height = c.dec->height;

    c.pkt = av_packet_alloc();
    c.frame = av_frame_alloc();
    if (!c.pkt || !c.frame) return out;

    StereoVote vote;
    const char* metaWhy = "";
    bool done = false;

    auto overBudget = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0)
                   .count() >= budgetMs;
    };

    while (!done && av_read_frame(c.fmt, c.pkt) >= 0) {
        if (c.pkt->stream_index != stream) { av_packet_unref(c.pkt); continue; }
        if (avcodec_send_packet(c.dec, c.pkt) < 0) { av_packet_unref(c.pkt); continue; }
        av_packet_unref(c.pkt);

        while (avcodec_receive_frame(c.dec, c.frame) >= 0) {
            ++out.framesExamined;

            if (!out.haveMeta) {
                StereoLayout ml = StereoLayout::Mono;
                bool inv = false;
                if (ReadStereo3D(c.frame, ml, inv, metaWhy)) {
                    out.haveMeta = true;
                    out.metaLayout = ml;
                    out.metaInvert = inv;
                }
            }
            // Metadata is authoritative — once we have it there is nothing to detect.
            // Same when the caller only wanted the metadata pass.
            if (out.haveMeta || !wantContent) { done = true; break; }

            // Never trust frame 0: an opening fade is exactly the flat-frame case, and
            // skipping dark frames (rather than letting them vote) is what stops it from
            // deciding the layout of the whole clip.
            if (!FrameIsDark(c.frame)) {
                const StereoDetectResult r = AnalyzeFrame(c.frame);
                vote.Add(r);
            }
            av_frame_unref(c.frame);

            if (vote.Samples() >= StereoVote::kVoteWant) { done = true; break; }
            if (out.framesExamined >= kMaxFrames || overBudget()) { done = true; break; }
        }
        av_frame_unref(c.frame);
        if (out.framesExamined >= kMaxFrames || overBudget()) done = true;
    }

    if (wantContent && !out.haveMeta) {
        // A stream that yielded only one or two usable frames would otherwise never reach
        // the vote threshold; accept what we have rather than regress single-frame clips.
        if (vote.Samples() > 0 && !vote.Sufficient() && out.framesExamined < kMaxFrames &&
            !overBudget()) {
            vote.AcceptSingle();   // the source ran out, not the budget
        }
        out.content = vote.Result();
        out.votes = vote.Samples();
    }

    out.elapsedMs =
        std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    LOG_INFO("VideoStereoProbe: '%s' %dx%d frames=%d votes=%d meta=%s%s content=%s(%s) %.0fms",
             path.c_str(), out.width, out.height, out.framesExamined, out.votes,
             out.haveMeta ? metaWhy : "none", out.metaInvert ? " R|L" : "",
             out.content.decided ? LayoutName(out.content.layout) : "undecided",
             out.content.reason, out.elapsedMs);
    return out;
}

#endif  // MEDIAPLAYER_WITH_FFMPEG

} // namespace mp
