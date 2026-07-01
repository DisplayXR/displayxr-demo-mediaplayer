// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Thin NativeActivity wrapper for the vendor-neutral media player. Three jobs:
//   1. Push the authoritative 4-way display rotation to native on launch and on
//      every rotation (incl. 180° flips, via a DisplayListener) — the renderer
//      can't derive true rotation from its own surface, and
//      Configuration.orientation only distinguishes portrait/landscape.
//   2. Forward touch from the runtime's MonadoView overlay (which covers our
//      window and is the only view that receives touch) to native via
//      dispatchTouchEvent (runtime#499) — a NativeActivity's native input queue
//      is NOT fed by dispatchTouchEvent. Native hit-tests the transport bar and
//      returns 1 when the Load button asks Java to open the SAF video picker.
//   3. Wake the DisplayXR runtime out of Android's "stopped" state before
//      xrCreateInstance and, if it can't be reached, prompt the user.
//
// Vendor-neutral + out-of-process (ADR-025): no CNSDK, no CAMERA — the OOP
// runtime service owns eye tracking. Binds to whichever runtime flavor is
// installed (out_of_process preferred, in_process dev fallback).

package com.displayxr.mediaplayer_vk_android

import android.app.AlertDialog
import android.app.NativeActivity
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.widget.Toast

class MainActivity : NativeActivity() {

    companion object {
        private const val TAG = "mediaplayer_vk_android"
        private const val REQUEST_PICK_VIDEO = 2

        // Load the native lib into the JVM so the external JNI functions below
        // resolve (NativeActivity also dlopens it for android_main; this load
        // is what binds the Java_… symbols).
        init {
            System.loadLibrary("mediaplayer_vk_android")
        }

        // Runtime flavors, in discovery preference order. ADR-025: the
        // out-of-process runtime is production; in_process is dev-only.
        private val RUNTIME_PACKAGES = arrayOf(
            "org.freedesktop.monado.openxr_runtime.out_of_process",
            "org.freedesktop.monado.openxr_runtime.in_process",
        )
    }

    // Implemented in main.cpp. rotation = Surface.ROTATION_0/90/180/270 → 0/1/2/3;
    // portrait = the ACTUAL held orientation (window taller than wide). The
    // rotation→orientation mapping is device-dependent (this panel's natural
    // orientation is portrait, so ROTATION_90 is landscape), so native must NOT
    // infer orientation from the rotation parity — it uses this flag directly to
    // pick the per-eye tile dims.
    private external fun nativeSetRotation(rotation: Int, portrait: Boolean)

    // True once xrCreateInstance failed with RUNTIME_UNAVAILABLE.
    private external fun nativeRuntimeUnavailable(): Boolean

    // True once the OpenXR instance is up (runtime reached).
    private external fun nativeXrReady(): Boolean

    // Hand a picked video to native as an open fd + byte range (AMediaExtractor
    // reads the fd). Reached by tapping the on-screen Load button.
    private external fun nativeOpenVideoFd(fd: Int, offset: Long, length: Long)

    // Raw touch (normalized coords) → native hit-tests the on-screen transport
    // bar (play/pause, scrub, load). Returns 1 when the Load button was tapped —
    // only Java can launch the system file picker.
    private external fun nativeTouch(action: Int, nx: Float, ny: Float): Int

    // Picker dismissed without a selection — native resumes the previous asset
    // (it blanks the scene while a pick is pending to avoid a stale-frame flash).
    private external fun nativePickCancelled()


    // First installed runtime package, preferring out_of_process. Null if none.
    private val installedRuntime: String? by lazy {
        RUNTIME_PACKAGES.firstOrNull {
            try {
                packageManager.getLaunchIntentForPackage(it) != null ||
                    packageManager.getPackageInfo(it, 0) != null
            } catch (_: Throwable) {
                false
            }
        }
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        if (event.pointerCount >= 1) {
            val dm = resources.displayMetrics
            val nx = event.x / dm.widthPixels.toFloat()
            val ny = event.y / dm.heightPixels.toFloat()
            try {
                if (nativeTouch(event.actionMasked, nx, ny) == 1) openVideoPicker()
            } catch (_: Throwable) {
                // Native lib not bound yet — ignore until it is.
            }
        }
        return super.dispatchTouchEvent(event)
    }

    private fun openVideoPicker() {
        try {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                // Videos AND images: stereo LIFs ship as .jpg, plain SBS images
                // as jpg/png. Native sniffs the picked fd's content and routes
                // image vs video accordingly.
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("video/*", "image/*"))
                // Persistable so the relaunched process can reopen it at cold start.
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
            }
            startActivityForResult(intent, REQUEST_PICK_VIDEO)
        } catch (_: Throwable) {
        }
    }

    @Deprecated("startActivityForResult is fine for a NativeActivity demo app")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        android.util.Log.i(TAG, "onActivityResult req=$requestCode res=$resultCode uri=${data?.data}")
        if (requestCode == REQUEST_PICK_VIDEO) {
            val uri = if (resultCode == RESULT_OK) data?.data else null
            if (uri == null) {
                // Cancelled (or no uri): let native resume the previous asset.
                try {
                    nativePickCancelled()
                } catch (_: Throwable) {
                }
                return
            }
            try {
                val pfd = contentResolver.openFileDescriptor(uri, "r") ?: return
                val length = pfd.statSize
                nativeOpenVideoFd(pfd.detachFd(), 0L, length)
                android.util.Log.i(TAG, "picked fd handed to native (len=$length)")
            } catch (t: Throwable) {
                android.util.Log.e(TAG, "openFileDescriptor failed", t)
                try {
                    nativePickCancelled()
                } catch (_: Throwable) {
                }
            }
        }
    }

    // Watch native bring-up just until it resolves: if the runtime can't be
    // reached, prompt to launch DisplayXR; if it comes up, stop. Bounded poll.
    private fun watchForRuntimeUnavailable() {
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed(
            object : Runnable {
                var tries = 0
                override fun run() {
                    if (isFinishing) return
                    val unavailable = try { nativeRuntimeUnavailable() } catch (_: Throwable) { false }
                    if (unavailable) {
                        showRuntimeMissingDialog()
                        return
                    }
                    val ready = try { nativeXrReady() } catch (_: Throwable) { false }
                    if (ready) return
                    if (tries++ < 15) handler.postDelayed(this, 1000)
                }
            },
            2000,
        )
    }

    private fun showRuntimeMissingDialog() {
        try {
            AlertDialog.Builder(this)
                .setTitle("DisplayXR not running")
                .setMessage(
                    "Couldn't reach the DisplayXR runtime.\n\n" +
                        "Open the DisplayXR app once (it shows the logo), then reopen this app.",
                )
                .setCancelable(false)
                .setPositiveButton("Open DisplayXR") { _, _ ->
                    installedRuntime?.let { pkg ->
                        packageManager.getLaunchIntentForPackage(pkg)?.let { startActivity(it) }
                    }
                    finish()
                }
                .setNegativeButton("Close") { _, _ -> finish() }
                .show()
        } catch (_: Throwable) {
        }
    }

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayChanged(displayId: Int) = pushRotation()
        override fun onDisplayAdded(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) {}
    }

    private fun pushRotation() {
        @Suppress("DEPRECATION")
        val rotation = windowManager.defaultDisplay.rotation // Surface.ROTATION_*
        // True held orientation from the window itself (taller than wide), NOT
        // inferred from the rotation parity — this panel's natural orientation
        // is portrait, so ROTATION_90 is landscape; the parity is device-
        // dependent but the window dims are not.
        val dm = resources.displayMetrics
        val portrait = dm.heightPixels > dm.widthPixels
        try {
            nativeSetRotation(rotation, portrait)
        } catch (_: Throwable) {
            // Native lib not bound yet — a later display/config change retries.
        }
    }

    // Wake the runtime package before xrCreateInstance. After a force-stop /
    // fresh install the runtime is in Android's "stopped" state, so the loader's
    // broker lookup excludes it → RUNTIME_UNAVAILABLE on a cold tap. An explicit
    // intent with FLAG_INCLUDE_STOPPED_PACKAGES clears the stopped flag so the
    // broker becomes discoverable. (Real apps assume the runtime already ran.)
    private fun wakeRuntime() {
        val pkg = installedRuntime ?: return
        try {
            val intent = Intent("org.khronos.openxr.OpenXRRuntimeService").apply {
                `package` = pkg
                addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES)
            }
            startService(intent)
        } catch (_: Throwable) {
            // Best-effort; the native side retries xrCreateInstance.
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        wakeRuntime()
        super.onCreate(savedInstanceState)
        pushRotation()
        (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
            .registerDisplayListener(displayListener, null)
        watchForRuntimeUnavailable()
        showControlsHint()
    }

    // Brief on-screen legend of the transport controls. A Toast sits above the
    // weave overlay, so no Vulkan HUD work is needed for a transient hint.
    private fun showControlsHint() {
        Handler(Looper.getMainLooper()).postDelayed({
            if (!isFinishing) {
                Toast.makeText(
                    this,
                    "Tap the screen to open a video / image / LIF",
                    Toast.LENGTH_LONG,
                ).show()
            }
        }, 2500)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        pushRotation()
    }

    override fun onResume() {
        super.onResume()
        pushRotation()
    }

    override fun onDestroy() {
        try {
            (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
                .unregisterDisplayListener(displayListener)
        } catch (_: Throwable) {
        }
        super.onDestroy()
    }
}
