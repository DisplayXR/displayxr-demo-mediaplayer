// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package com.displayxr.mediaplayer_vk_android.media3

import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.Format
import androidx.media3.common.MimeTypes
import androidx.media3.common.Timeline
import androidx.media3.common.TrackGroup
import androidx.media3.common.Tracks
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.RendererCapabilities
import androidx.media3.exoplayer.RendererConfiguration
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.exoplayer.source.TrackGroupArray
import androidx.media3.exoplayer.trackselection.ExoTrackSelection
import androidx.media3.exoplayer.trackselection.FixedTrackSelection
import androidx.media3.exoplayer.trackselection.TrackSelector
import androidx.media3.exoplayer.trackselection.TrackSelectorResult

/**
 * Assigns the two eye tracks of a dual-track stereo MP4 to the two video
 * renderers (#71, finding 2).
 *
 * `DefaultTrackSelector` selects ONE track per type and `MappingTrackSelector`
 * maps every video group onto the first video renderer (its `selectTracks` is
 * final and `preferUnassociatedRenderer` is hard-coded to metadata), so this
 * implements [TrackSelector] directly — the pattern the maintainers pointed at in
 * google/ExoPlayer#6589 and re-confirmed in androidx/media#1473.
 *
 * Eye identification mirrors the native probe: `mdhd` language `abl`/`abr`
 * (→ [Format.language]) first, then track order. `tkhd` disabled flags are not
 * read by the extractor, so both tracks are always visible. A mono file enables
 * only the left renderer; the right one stays disabled and its surface idle.
 */
@OptIn(UnstableApi::class)
class StereoTrackSelector : TrackSelector() {

    /** What the last selection decided, for the native open flow to read. */
    @Volatile var lastSelection: Selection = Selection.NONE
        private set

    data class Selection(val leftTrackId: String?, val rightTrackId: String?, val audioTrackId: String?) {
        val isDual: Boolean get() = leftTrackId != null && rightTrackId != null
        companion object { val NONE = Selection(null, null, null) }
    }

    override fun selectTracks(
        rendererCapabilities: Array<RendererCapabilities>,
        trackGroups: TrackGroupArray,
        periodId: MediaSource.MediaPeriodId,
        timeline: Timeline,
    ): TrackSelectorResult {
        val video = ArrayList<TrackGroup>()
        var audioGroup: TrackGroup? = null
        for (i in 0 until trackGroups.length) {
            val g = trackGroups[i]
            when (MimeTypes.getTrackType(g.getFormat(0).sampleMimeType)) {
                C.TRACK_TYPE_VIDEO -> video.add(g)
                C.TRACK_TYPE_AUDIO -> if (audioGroup == null) audioGroup = g
                else -> Unit  // metadata / unknown (the mett convergence track): native owns it
            }
        }

        val (left, right) = pickEyes(video)

        val selections = arrayOfNulls<ExoTrackSelection>(rendererCapabilities.size)
        val configs = arrayOfNulls<RendererConfiguration>(rendererCapabilities.size)
        fun enable(index: Int, group: TrackGroup?) {
            if (group == null || index >= rendererCapabilities.size) return
            selections[index] = FixedTrackSelection(group, 0)
            configs[index] = RendererConfiguration.DEFAULT
        }
        enable(StereoRenderersFactory.INDEX_VIDEO_LEFT, left)
        enable(StereoRenderersFactory.INDEX_VIDEO_RIGHT, right)
        enable(StereoRenderersFactory.INDEX_AUDIO, audioGroup)

        lastSelection = Selection(left?.id(), right?.id(), audioGroup?.id())

        val groups = ArrayList<Tracks.Group>(trackGroups.length)
        for (i in 0 until trackGroups.length) {
            val g = trackGroups[i]
            val selected = g === left || g === right || g === audioGroup
            groups.add(Tracks.Group(
                g,
                /* adaptiveSupported= */ false,
                IntArray(g.length) { C.FORMAT_HANDLED },
                BooleanArray(g.length) { it == 0 && selected }))
        }
        return TrackSelectorResult(configs, selections, Tracks(groups), /* info= */ null)
    }

    override fun onSelectionActivated(info: Any?) = Unit

    /**
     * Left = the group whose language is `abl`, right = `abr`; if neither tag is
     * present, the first two video groups in file order. One video group = mono.
     */
    private fun pickEyes(video: List<TrackGroup>): Pair<TrackGroup?, TrackGroup?> {
        if (video.isEmpty()) return null to null
        if (video.size == 1) return video[0] to null
        val byLang = video.associateBy { it.getFormat(0).language?.lowercase() }
        val l = byLang["abl"]
        val r = byLang["abr"]
        if (l != null && r != null) return l to r
        return video[0] to video[1]
    }

    private fun TrackGroup.id(): String? = getFormat(0).id
}
