// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// The Java half of the media browser's MediaProvider (#160, docs/BROWSER.md): reads the
// MediaStore, asks for the read permission, makes thumbnails and opens files. Native
// (browser/AndroidMediaProvider.cpp) never calls in here: poll() takes its requests once
// per Choreographer frame, and results go back through the native* calls (runtime#507).

package com.displayxr.mediaplayer_vk_android

import android.Manifest
import android.app.Activity
import android.content.ContentUris
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import android.provider.Settings
import android.util.Log
import android.util.Size
import java.io.FileNotFoundException
import java.io.IOException
import java.nio.ByteBuffer
import java.util.concurrent.Executors
import kotlin.math.max
import kotlin.math.roundToInt

class MediaStoreProvider(
    private val activity: Activity,
    // Hands an opened file to the player, as the system picker's result does.
    private val openFd: (fd: Int, length: Long, name: String) -> Unit,
) {
    companion object {
        private const val TAG = "mediaplayer_vk_android"
        const val REQUEST_PERMISSION = 3

        // MediaAccess in MediaProvider.h.
        private const val ACCESS_NONE = 1
        private const val ACCESS_PARTIAL = 2
        private const val ACCESS_FULL = 3
        private const val ACCESS_DENIED = 4

        // MediaType in MediaItem.h.
        private const val TYPE_IMAGE = 0
        private const val TYPE_VIDEO = 1

        // 4, not 2: 272 thumbnails in 1.8 s instead of 2.4 s on the NP02J.
        private const val THUMBNAIL_THREADS = 4
        private const val PREFS = "media_browser"
        private const val PREF_ASKED = "permission_asked"
    }

    private class Row(
        val id: Long,
        val uri: Uri,
        val name: String,
        val type: Int,
        val dateMs: Long,
        val modified: Long,
        val size: Long,
        val durationMs: Long,
        val folder: String,
    )

    private external fun nativeSetAccess(access: Int)
    private external fun nativeSetItems(
        ids: LongArray,
        names: Array<String>,
        types: IntArray,
        dates: LongArray,
        modified: LongArray,
        sizes: LongArray,
        durations: LongArray,
        folders: Array<String>,
    )
    // `fd` is the file itself (or -1), for native to check whether it is 3D. Native closes it.
    private external fun nativeThumbnail(id: Long, width: Int, height: Int, rgba: ByteArray?, fd: Int, video: Boolean)
    private external fun nativeTakeAccessRequest(): Boolean
    private external fun nativeTakeRefreshRequest(): Boolean
    // Pairs of (id, 1 to check the file or 0), or null if unchanged.
    private external fun nativeTakeWantedThumbnails(edgeOut: IntArray): LongArray?
    private external fun nativeTakeOpenRequest(): Long

    private val queryThread = Executors.newSingleThreadExecutor()
    private val thumbnailPool = Executors.newFixedThreadPool(THUMBNAIL_THREADS)
    @Volatile private var rows: Map<Long, Row> = emptyMap()
    private var listed = false // native has asked for the list, so keep it fresh on resume

    // Thumbnails. `made` holds what was made for ids still wanted, so an id is not made
    // twice while its thumbnail is on its way to native. Guarded by `lock`.
    private val lock = Any()
    private var wanted = LongArray(0)
    private var checkFile = HashSet<Long>()
    private var wantedEdge = 0
    private val inFlight = HashSet<Long>()
    private val made = HashSet<Long>()
    private var workers = 0
    private val edgeOut = IntArray(1)

    /** Once per Choreographer frame, on the UI thread. */
    fun poll() {
        if (nativeTakeAccessRequest()) requestAccess()
        if (nativeTakeRefreshRequest()) refresh()
        nativeTakeWantedThumbnails(edgeOut)?.let { pairs ->
            val ids = LongArray(pairs.size / 2) { pairs[2 * it] }
            val check = (0 until pairs.size / 2).filter { pairs[2 * it + 1] != 0L }.mapTo(HashSet()) { pairs[2 * it] }
            setWanted(ids, check, edgeOut[0])
        }
        val open = nativeTakeOpenRequest()
        if (open >= 0) open(open)
    }

    fun onResume() {
        nativeSetAccess(access())
        if (listed) refresh() // the library or the granted selection may have changed
    }

    fun onPermissionResult() {
        nativeSetAccess(access())
        if (listed) refresh()
    }

    fun close() {
        queryThread.shutdownNow()
        thumbnailPool.shutdownNow()
    }

    // ---------------------------------------------------------------- access

    private fun permissions(): Array<String> = when {
        Build.VERSION.SDK_INT >= 34 -> arrayOf(
            Manifest.permission.READ_MEDIA_IMAGES,
            Manifest.permission.READ_MEDIA_VIDEO,
            Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED,
        )
        Build.VERSION.SDK_INT >= 33 ->
            arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VIDEO)
        else -> arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)
    }

    private fun granted(permission: String) =
        activity.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private fun asked() =
        activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(PREF_ASKED, false)

    private fun access(): Int {
        val full = if (Build.VERSION.SDK_INT >= 33) {
            granted(Manifest.permission.READ_MEDIA_IMAGES) && granted(Manifest.permission.READ_MEDIA_VIDEO)
        } else {
            granted(Manifest.permission.READ_EXTERNAL_STORAGE)
        }
        return when {
            full -> ACCESS_FULL
            Build.VERSION.SDK_INT >= 34 && granted(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED) ->
                ACCESS_PARTIAL
            // Once refused for good, Android shows no prompt and gives no rationale. A
            // first launch gives no rationale either, hence the stored "asked" flag.
            asked() && permissions().none { activity.shouldShowRequestPermissionRationale(it) } ->
                ACCESS_DENIED
            else -> ACCESS_NONE
        }
    }

    private fun requestAccess() {
        if (access() == ACCESS_DENIED) {
            activity.startActivity(
                Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", activity.packageName, null)),
            )
            return
        }
        activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(PREF_ASKED, true).apply()
        // On Android 14 with partial access, asking again offers "select more".
        activity.requestPermissions(permissions(), REQUEST_PERMISSION)
    }

    // ---------------------------------------------------------------- the list

    private fun refresh() {
        listed = true
        queryThread.execute {
            val list = ArrayList<Row>()
            query(MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL), TYPE_IMAGE, list)
            query(MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL), TYPE_VIDEO, list)
            rows = list.associateBy { it.id }
            nativeSetItems(
                LongArray(list.size) { list[it].id },
                Array(list.size) { list[it].name },
                IntArray(list.size) { list[it].type },
                LongArray(list.size) { list[it].dateMs },
                LongArray(list.size) { list[it].modified },
                LongArray(list.size) { list[it].size },
                LongArray(list.size) { list[it].durationMs },
                Array(list.size) { list[it].folder },
            )
            Log.i(TAG, "media browser: ${list.size} items")
        }
    }

    private fun query(collection: Uri, type: Int, out: MutableList<Row>) {
        val columns = arrayOf(
            MediaStore.MediaColumns._ID,
            MediaStore.MediaColumns.DISPLAY_NAME,
            MediaStore.MediaColumns.DATE_TAKEN,
            MediaStore.MediaColumns.DATE_MODIFIED,
            MediaStore.MediaColumns.SIZE,
            MediaStore.MediaColumns.DURATION,
            MediaStore.MediaColumns.RELATIVE_PATH,
        )
        activity.contentResolver.query(collection, columns, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                val id = c.getLong(0)
                val name = c.getString(1) ?: continue
                val taken = c.getLong(2)
                val modified = c.getLong(3)
                out += Row(
                    id, ContentUris.withAppendedId(collection, id), name, type,
                    if (taken > 0) taken else modified * 1000, modified, c.getLong(4), c.getLong(5),
                    c.getString(6) ?: "",
                )
            }
        }
    }

    // ---------------------------------------------------------------- thumbnails

    private fun setWanted(ids: LongArray, check: HashSet<Long>, edge: Int) {
        synchronized(lock) {
            wanted = ids
            checkFile = check
            wantedEdge = edge
            made.retainAll(ids.toHashSet())
            while (ids.isNotEmpty() && workers < THUMBNAIL_THREADS) {
                workers++
                thumbnailPool.execute { makeThumbnails() }
            }
        }
    }

    // A worker: makes the most wanted thumbnail nobody has started, until none is left.
    private fun makeThumbnails() {
        while (true) {
            val id: Long
            val edge: Int
            val check: Boolean
            synchronized(lock) {
                val next = wanted.firstOrNull { it !in inFlight && it !in made }
                if (next == null) {
                    workers--
                    return
                }
                id = next
                edge = wantedEdge
                check = id in checkFile
                inFlight += id
            }
            val row = rows[id]
            val bitmap = row?.let { thumbnail(it, edge) }
            val rgba = bitmap?.let {
                val pixels = ByteBuffer.allocate(it.width * it.height * 4)
                it.copyPixelsToBuffer(pixels) // ARGB_8888 is laid out R, G, B, A in memory
                pixels.array()
            }
            synchronized(lock) {
                inFlight -= id
                made += id
            }
            nativeThumbnail(
                id, bitmap?.width ?: 0, bitmap?.height ?: 0, rgba,
                if (check && row != null) detachedFd(row) else -1, row?.type == TYPE_VIDEO,
            )
        }
    }

    private fun detachedFd(row: Row): Int = try {
        activity.contentResolver.openFileDescriptor(row.uri, "r")?.detachFd() ?: -1
    } catch (e: FileNotFoundException) {
        -1
    }

    private fun thumbnail(row: Row, edge: Int): Bitmap? {
        var bitmap = try {
            activity.contentResolver.loadThumbnail(row.uri, Size(edge, edge), null)
        } catch (e: IOException) {
            Log.w(TAG, "no thumbnail for ${row.name}: $e")
            return null
        }
        if (bitmap.config != Bitmap.Config.ARGB_8888) bitmap = bitmap.copy(Bitmap.Config.ARGB_8888, false)
        // loadThumbnail samples by powers of two, so it can return up to twice the size asked for.
        val scale = edge.toFloat() / max(bitmap.width, bitmap.height)
        if (scale < 1f) {
            bitmap = Bitmap.createScaledBitmap(
                bitmap,
                max(1, (bitmap.width * scale).roundToInt()),
                max(1, (bitmap.height * scale).roundToInt()),
                true,
            )
        }
        return bitmap
    }

    // ---------------------------------------------------------------- open

    private fun open(id: Long) {
        val row = rows[id]
        if (row == null) {
            Log.w(TAG, "media browser: no item $id to open")
            return
        }
        try {
            val pfd = activity.contentResolver.openFileDescriptor(row.uri, "r") ?: return
            val length = pfd.statSize
            openFd(pfd.detachFd(), length, row.name)
        } catch (e: FileNotFoundException) {
            Log.w(TAG, "media browser: cannot open ${row.name}: $e")
        }
    }
}
