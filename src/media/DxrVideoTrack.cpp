// SPDX-License-Identifier: Apache-2.0
#include "DxrVideoTrack.h"

#if MEDIAPLAYER_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
#endif

namespace mp::dxrfile {

#if MEDIAPLAYER_WITH_FFMPEG
int ReadConvergenceTrack(const std::string& path, uint32_t trackId, ConvergenceCurve& curve) {
    curve.Clear();
    if (trackId == 0) return 0;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return 0;
    int stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        const bool ours = st->id == (int)trackId && st->codecpar->codec_type == AVMEDIA_TYPE_DATA;
        st->discard = ours ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        if (ours && stream < 0) stream = (int)i;
    }
    int read = 0;
    if (stream >= 0) {
        const AVRational tb = fmt->streams[stream]->time_base;
        AVPacket* pkt = av_packet_alloc();
        // One sample per frame: a 10-hour clip at 60 fps is 2.2 M samples. Bound it anyway.
        constexpr int kMaxSamples = 4 * 1000 * 1000;
        while (pkt && read < kMaxSamples && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == stream) {
                const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                if (ts != AV_NOPTS_VALUE) {
                    curve.Add(av_rescale_q(ts, tb, AVRational{1, 1000000}), pkt->data, (size_t)pkt->size);
                    ++read;
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    avformat_close_input(&fmt);
    return read;
}
#else
int ReadConvergenceTrack(const std::string&, uint32_t, ConvergenceCurve& curve) {
    curve.Clear();
    return 0;
}
#endif

}  // namespace mp::dxrfile
