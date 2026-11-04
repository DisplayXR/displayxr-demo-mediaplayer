// SPDX-License-Identifier: Apache-2.0
//
// SpatialFFmpeg (#154) — the libav side of SpatialMedia, header-only so the two desktop
// translation units that need it (VideoDecoder.cpp, VideoStereoProbe.cpp) share one
// definition without a new .cpp in the build lists. Include it only from code that is
// already built with FFmpeg (MEDIAPLAYER_WITH_FFMPEG).
//
// FFmpeg reads MV-HEVC from 7.1 on (libavcodec 61.19 / libavformat 61.7): the mov demuxer
// marks the stream AV_DISPOSITION_MULTILAYER when the sample entry carries the layered
// configuration, and the HEVC decoder outputs every view when its `view_ids` option is -1,
// tagging each frame with AV_FRAME_DATA_VIEW_ID and an AVStereo3D whose `view` names the
// eye. The same release grew HEIF item / tile-grid demuxing. Older FFmpeg (Ubuntu 22.04's
// 4.4, 24.04's 6.1) builds this as "not available": MV-HEVC then plays its base view flat,
// exactly as before, and .heic is not offered.
#pragma once

#include "SpatialMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/stereo3d.h>
}

#include <cstring>

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100) && \
    LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(61, 7, 100)
#define MP_HAVE_FF_SPATIAL 1
#else
#define MP_HAVE_FF_SPATIAL 0
#endif

namespace mp::spatial::ff {

// A stereo MV-HEVC track: HEVC with more than one coded layer.
inline bool IsMvHevcStream(const AVStream* st) {
#if MP_HAVE_FF_SPATIAL
    return st && st->codecpar && st->codecpar->codec_id == AV_CODEC_ID_HEVC &&
           (st->disposition & AV_DISPOSITION_MULTILAYER) != 0;
#else
    (void)st;
    return false;
#endif
}

// The eye a decoded view says it is (AVStereo3D.view), Unknown when it says nothing.
inline EyeHint HintOf(const AVFrame* f) {
    const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_STEREO3D);
    if (!sd || (size_t)sd->size < sizeof(AVStereo3D)) return EyeHint::Unknown;
    const AVStereo3D* s3 = reinterpret_cast<const AVStereo3D*>(sd->data);
    if (s3->view == AV_STEREO3D_VIEW_LEFT) return EyeHint::Left;
    if (s3->view == AV_STEREO3D_VIEW_RIGHT) return EyeHint::Right;
    return EyeHint::Unknown;
}

// The view id of a decoded MV-HEVC view, -1 when the frame carries none.
inline int ViewIdOf(const AVFrame* f) {
#if MP_HAVE_FF_SPATIAL
    const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_VIEW_ID);
    if (sd && (size_t)sd->size >= sizeof(int)) {
        int v = 0;
        std::memcpy(&v, sd->data, sizeof(int));
        return v;
    }
#else
    (void)f;
#endif
    return -1;
}

// Glue two same-sized, same-format SOFTWARE frames side by side into `out` (left | right),
// in their native pixel format — planar or semi-planar YUV at any bit depth — so the
// decoder's normal conversion path takes the result unchanged. `out` gets L's timing and
// colour properties. Fails (false) on mismatched views, hardware frames, or a width that a
// subsampled chroma plane cannot split evenly (odd width with 4:2:x chroma).
inline bool ComposeSideBySide(const AVFrame* L, const AVFrame* R, AVFrame* out) {
    if (!L || !R || !out || L->format != R->format || L->width != R->width || L->height != R->height ||
        L->width <= 0 || L->height <= 0)
        return false;
    const AVPixelFormat pf = (AVPixelFormat)L->format;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf);
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM)))
        return false;
    if ((L->width & ((1 << d->log2_chroma_w) - 1)) != 0) return false;

    av_frame_unref(out);
    out->format = pf;
    out->width = L->width * 2;
    out->height = L->height;
    if (av_frame_get_buffer(out, 32) < 0) return false;
    const int planes = av_pix_fmt_count_planes(pf);
    for (int p = 0; p < planes; ++p) {
        const int rowBytes = av_image_get_linesize(pf, L->width, p);
        if (rowBytes <= 0) { av_frame_unref(out); return false; }
        const bool chroma = (p == 1 || p == 2) && !(d->flags & AV_PIX_FMT_FLAG_RGB);
        const int rows = chroma ? AV_CEIL_RSHIFT(L->height, d->log2_chroma_h) : L->height;
        for (int y = 0; y < rows; ++y) {
            uint8_t* dst = out->data[p] + (ptrdiff_t)y * out->linesize[p];
            std::memcpy(dst, L->data[p] + (ptrdiff_t)y * L->linesize[p], (size_t)rowBytes);
            std::memcpy(dst + rowBytes, R->data[p] + (ptrdiff_t)y * R->linesize[p], (size_t)rowBytes);
        }
    }
    av_frame_copy_props(out, L);
    // The per-view tags describe ONE view; the composite is a packed pair.
    av_frame_remove_side_data(out, AV_FRAME_DATA_STEREO3D);
#if MP_HAVE_FF_SPATIAL
    av_frame_remove_side_data(out, AV_FRAME_DATA_VIEW_ID);
#endif
    return true;
}

}  // namespace mp::spatial::ff
