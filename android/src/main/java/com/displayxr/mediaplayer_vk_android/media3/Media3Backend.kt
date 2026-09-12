// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package com.displayxr.mediaplayer_vk_android.media3

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.Renderer
import androidx.media3.exoplayer.SeekParameters
import java.nio.ByteBuffer

/**
 * The ExoPlayer producer behind the native AImageReader consumer (#71).
 *
 * Owns one [ExoPlayer] on a dedicated looper thread (so every call here is on
 * the player's application thread and `verifyApplicationThread` is satisfied
 * without touching the UI thread). The native side talks to it through the
 * small surface below; per-frame it reads only the direct clock buffer.
 *
 * SPIKE STATUS: compiles; not yet wired to native. Phase 1 of #71 wires
 * [setSurfaces] to `ANativeWindow_toSurface(AImageReader_getWindow(...))` and
 * asserts on-device that `AImage_getTimestamp()` carries the media PTS.
 */
@OptIn(UnstableApi::class)
class Media3Backend(context: Context, private val callbacks: Callbacks) {

    interface Callbacks {
        /** Tracks resolved; `dual` = both eye renderers enabled. */
        fun onPrepared(durationUs: Long, width: Int, height: Int, frameRate: Float, dual: Boolean)
        fun onEnded()
        fun onError(code: Int, message: String)
    }

    private val appContext = context.applicationContext
    private val thread = HandlerThread("media3-backend").apply { start() }
    private val handler = Handler(thread.looper)
    private val renderers = StereoRenderersFactory(appContext)
    private val selector = StereoTrackSelector()
    private lateinit var player: ExoPlayer

    /** Direct buffer the native clock reader maps; valid after [create]. */
    lateinit var clockAnchor: ByteBuffer
        private set

    init {
        runOnPlayer { create() }
    }

    private fun create() {
        player = ExoPlayer.Builder(appContext, renderers)
            .setTrackSelector(selector)
            .setLooper(thread.looper)
            .setSeekParameters(SeekParameters.EXACT)
            .build()
        clockAnchor = renderers.audio.anchor
        player.addListener(object : Player.Listener {
            override fun onPlaybackStateChanged(state: Int) {
                when (state) {
                    Player.STATE_READY -> if (!preparedReported) {
                        preparedReported = true
                        val f = player.videoFormat
                        callbacks.onPrepared(
                            player.duration * 1000L,
                            f?.width ?: 0, f?.height ?: 0, f?.frameRate ?: 0f,
                            selector.lastSelection.isDual)
                    }
                    Player.STATE_ENDED -> callbacks.onEnded()
                    else -> Unit
                }
            }
            override fun onPlayerError(error: PlaybackException) {
                Log.e(TAG, "player error ${error.errorCodeName}", error)
                callbacks.onError(error.errorCode, error.message ?: error.errorCodeName)
            }
        })
    }

    @Volatile private var preparedReported = false

    // ── native-facing surface ─────────────────────────────────────────────

    /** Open a SAF `content://` (or `file://`) URI. The persistable grant must already be held. */
    fun open(uri: String, loop: Boolean) = runOnPlayer {
        preparedReported = false
        player.setMediaItem(MediaItem.fromUri(Uri.parse(uri)))
        player.repeatMode = if (loop) Player.REPEAT_MODE_ONE else Player.REPEAT_MODE_OFF
        player.prepare()
    }

    /**
     * One surface per eye renderer. `setVideoSurface` would broadcast the same
     * surface to both video renderers; the per-renderer message is the only way
     * to give each eye its own AImageReader.
     */
    fun setSurfaces(left: Surface?, right: Surface?) = runOnPlayer {
        player.createMessage(renderers.videoLeft)
            .setType(Renderer.MSG_SET_VIDEO_OUTPUT).setPayload(left).send()
        player.createMessage(renderers.videoRight)
            .setType(Renderer.MSG_SET_VIDEO_OUTPUT).setPayload(right).send()
    }

    fun play() = runOnPlayer { player.playWhenReady = true }
    fun pause() = runOnPlayer { player.playWhenReady = false }
    fun seekToUs(positionUs: Long) = runOnPlayer { player.seekTo(positionUs / 1000L) }
    fun setLoop(loop: Boolean) = runOnPlayer {
        player.repeatMode = if (loop) Player.REPEAT_MODE_ONE else Player.REPEAT_MODE_OFF
    }
    fun stop() = runOnPlayer { player.stop(); player.clearMediaItems() }

    fun release() {
        runOnPlayer { player.release() }
        thread.quitSafely()
    }

    private fun runOnPlayer(block: () -> Unit) {
        if (Thread.currentThread() === thread) block() else handler.post(block)
    }

    companion object {
        private const val TAG = "Media3Backend"
        @Suppress("unused") const val TIME_UNSET = C.TIME_UNSET
    }
}
