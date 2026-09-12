// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package com.displayxr.mediaplayer_vk_android.media3

import android.content.Context
import android.os.Handler
import androidx.annotation.OptIn
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.mediacodec.MediaCodecAdapter
import androidx.media3.exoplayer.mediacodec.MediaCodecSelector
import androidx.media3.exoplayer.video.MediaCodecVideoRenderer
import androidx.media3.exoplayer.video.VideoRendererEventListener

/**
 * A video renderer that is a DECODE PUMP, not a presenter (#71, finding 1 + 3).
 *
 * Stock [MediaCodecVideoRenderer] releases each output buffer with a vsync-snapped
 * `System.nanoTime()` release time, which becomes the BufferQueue timestamp the
 * native consumer reads back through `AImage_getTimestamp()`. Our consumer needs
 * that timestamp to be the MEDIA PTS — it selects the frame for the runtime's
 * predicted display time and pairs left/right eyes on exact PTS. So:
 *
 *  - [renderOutputBufferV21] releases at `presentationTimeUs * 1000`, exactly what
 *    the native `AMediaCodec_releaseOutputBufferAtTime` path did. An AImageReader
 *    consumer never applies desired-present-time deferral, so the value is pure
 *    metadata.
 *  - Dropping is disabled in both hooks: two of these renderers drive the two eye
 *    tracks and each owns its own release control; letting either drop on its own
 *    is the one thing that can desync the eyes. The native pairing queue absorbs
 *    jitter instead.
 */
@OptIn(UnstableApi::class)
class PtsStampingVideoRenderer(
    context: Context,
    eventHandler: Handler?,
    eventListener: VideoRendererEventListener?,
    /** Which eye this renderer feeds; informational (logs, track assignment). */
    val eye: Int,
) : MediaCodecVideoRenderer(
    context,
    MediaCodecSelector.DEFAULT,
    /* allowedJoiningTimeMs= */ 0L,
    eventHandler,
    eventListener,
    /* maxDroppedFramesToNotify= */ 0,
) {
    override fun renderOutputBufferV21(
        codec: MediaCodecAdapter,
        index: Int,
        presentationTimeUs: Long,
        releaseTimeNs: Long,
    ) {
        // Through super, NOT codec.releaseOutputBuffer directly: the base method
        // passes releaseTimeNs straight to the codec (verified, 1.8.0) AND does the
        // bookkeeping that makes this renderer report ready -- rendered-frame
        // counters and the "first frame rendered" notification. Bypassing it left
        // the player flapping BUFFERING<->READY: audio advanced at ~0.27x real time
        // and frames came out in bursts (measured on the tablet, #71 phase 1).
        super.renderOutputBufferV21(codec, index, presentationTimeUs, presentationTimeUs * 1000L)
    }

    override fun shouldDropOutputBuffer(
        earlyUs: Long,
        elapsedRealtimeUs: Long,
        isLastBuffer: Boolean,
    ): Boolean = false

    override fun shouldDropBuffersToKeyframe(
        earlyUs: Long,
        elapsedRealtimeUs: Long,
        isLastBuffer: Boolean,
    ): Boolean = false

    companion object {
        const val EYE_LEFT = 0
        const val EYE_RIGHT = 1
    }
}
