// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package com.displayxr.mediaplayer_vk_android.media3

import android.content.Context
import android.os.Handler
import androidx.annotation.OptIn
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.audio.AudioRendererEventListener
import androidx.media3.exoplayer.audio.AudioSink
import androidx.media3.exoplayer.audio.MediaCodecAudioRenderer
import androidx.media3.exoplayer.mediacodec.MediaCodecSelector
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The player's master clock, published for a native render thread (#71, finding 6).
 *
 * `Player.getCurrentPosition()` is millisecond-granular, ~10 ms stale and
 * application-thread-only. The accurate value — `AudioTrack.getTimestamp`,
 * latency-corrected, via `DefaultAudioSink` — is what [getPositionUs] returns on
 * the playback thread every `doSomeWork` pass. We publish a (positionUs, nanoTime)
 * anchor into a direct [ByteBuffer] the native side maps once; it then extrapolates
 * with `clock_gettime(CLOCK_MONOTONIC)` (Java `System.nanoTime()` is the same
 * clock on Android) at zero JNI calls per frame.
 *
 * Layout (little-endian, 32 bytes):
 *   [0]  seq        (long)  odd while a write is in progress — seqlock
 *   [8]  positionUs (long)  media position at anchor
 *   [16] anchorNs   (long)  System.nanoTime() at anchor
 *   [24] speedX1000 (long)  playback speed * 1000 (1000 = real time)
 */
@OptIn(UnstableApi::class)
class ClockPublishingAudioRenderer(
    context: Context,
    eventHandler: Handler?,
    eventListener: AudioRendererEventListener?,
    audioSink: AudioSink,
) : MediaCodecAudioRenderer(
    context,
    MediaCodecSelector.DEFAULT,
    eventHandler,
    eventListener,
    audioSink,
) {
    /** Shared with native through `GetDirectBufferAddress`; never reallocated. */
    val anchor: ByteBuffer = ByteBuffer.allocateDirect(32).order(ByteOrder.LITTLE_ENDIAN)

    @Volatile private var speedX1000: Long = 1000L

    override fun getPositionUs(): Long {
        val pos = super.getPositionUs()
        publish(pos, System.nanoTime())
        return pos
    }

    override fun setPlaybackSpeed(currentPlaybackSpeed: Float, targetPlaybackSpeed: Float) {
        super.setPlaybackSpeed(currentPlaybackSpeed, targetPlaybackSpeed)
        speedX1000 = (currentPlaybackSpeed * 1000f).toLong()
    }

    private fun publish(positionUs: Long, nowNs: Long) {
        // Seqlock: native readers spin on an even, unchanged seq around the read.
        val seq = anchor.getLong(0)
        anchor.putLong(0, seq + 1)
        anchor.putLong(8, positionUs)
        anchor.putLong(16, nowNs)
        anchor.putLong(24, speedX1000)
        anchor.putLong(0, seq + 2)
    }
}
