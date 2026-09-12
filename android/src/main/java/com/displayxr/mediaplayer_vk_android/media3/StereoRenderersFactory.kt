// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package com.displayxr.mediaplayer_vk_android.media3

import android.content.Context
import android.os.Handler
import androidx.annotation.OptIn
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.Renderer
import androidx.media3.exoplayer.RenderersFactory
import androidx.media3.exoplayer.audio.AudioRendererEventListener
import androidx.media3.exoplayer.audio.DefaultAudioSink
import androidx.media3.exoplayer.metadata.MetadataOutput
import androidx.media3.exoplayer.text.TextOutput
import androidx.media3.exoplayer.video.VideoRendererEventListener

/**
 * Exactly three renderers: `[videoLeft, videoRight, audio]` (#71, finding 2).
 *
 * No text, metadata, image or camera-motion renderers: the convergence `mett`
 * track is parsed natively once at open (finding 5), and a metadata renderer
 * would only add a Java-looper callback per frame. Indices are fixed so the
 * track selector and the surface messages can address the eyes by position.
 */
@OptIn(UnstableApi::class)
class StereoRenderersFactory(private val context: Context) : RenderersFactory {
    lateinit var videoLeft: PtsStampingVideoRenderer
        private set
    lateinit var videoRight: PtsStampingVideoRenderer
        private set
    lateinit var audio: ClockPublishingAudioRenderer
        private set

    override fun createRenderers(
        eventHandler: Handler,
        videoRendererEventListener: VideoRendererEventListener,
        audioRendererEventListener: AudioRendererEventListener,
        textRendererOutput: TextOutput,
        metadataRendererOutput: MetadataOutput,
    ): Array<Renderer> {
        videoLeft = PtsStampingVideoRenderer(
            context, eventHandler, videoRendererEventListener, PtsStampingVideoRenderer.EYE_LEFT)
        videoRight = PtsStampingVideoRenderer(
            context, eventHandler, videoRendererEventListener, PtsStampingVideoRenderer.EYE_RIGHT)
        audio = ClockPublishingAudioRenderer(
            context, eventHandler, audioRendererEventListener,
            DefaultAudioSink.Builder(context).build())
        return arrayOf(videoLeft, videoRight, audio)
    }

    companion object {
        const val INDEX_VIDEO_LEFT = 0
        const val INDEX_VIDEO_RIGHT = 1
        const val INDEX_AUDIO = 2
    }
}
