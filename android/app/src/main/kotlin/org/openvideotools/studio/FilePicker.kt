// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.ClipData
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Log
import android.view.DragEvent
import android.view.View
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.app.ActivityCompat
import androidx.core.content.IntentCompat
import java.util.concurrent.Executors

/**
 * Storage Access Framework file opening (chapter 13 §13.4.4 item 3; WI-0.051). No storage permission is used: a
 * document comes from `ActivityResultContracts.OpenDocument()`, an `ACTION_VIEW`/`ACTION_SEND` intent or a
 * drag-and-drop, its URI permission is persisted where the provider allows it (recents), and
 * `ContentResolver.openFileDescriptor` → `ParcelFileDescriptor.detachFd()` hands the descriptor to
 * `ovt_file_open_fd` through [OvtNative.openFd], which owns it from then on.
 *
 * Construct it in `Activity.onCreate` (the result launcher must be registered before the Activity starts). The
 * open itself runs on a single background thread because a provider can be slow; [onOpened] is called on the main
 * thread with the new session handle and replaces (closes) the previous session.
 */
internal class FilePicker(
    private val activity: ComponentActivity,
    private val onOpened: (OpenedFile) -> Unit,
) {
    /** A file opened in the native core. [session] is the `ovt_jni_open_fd` handle. */
    class OpenedFile(
        val session: Long,
        val displayName: String,
    )

    private val launcher =
        activity.registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) open(uri, persist = true)
        }
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "ovt-open").apply { isDaemon = true } }

    /** Shows the system document picker. */
    fun pick() {
        launcher.launch(mimeTypes.toTypedArray())
    }

    /** Opens the file named by an `ACTION_VIEW` or `ACTION_SEND` intent; returns false for any other intent. */
    fun handleIntent(intent: Intent?): Boolean {
        val uri =
            when (intent?.action) {
                Intent.ACTION_VIEW -> intent.data
                Intent.ACTION_SEND -> IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)
                else -> null
            } ?: return false
        // The grant of a VIEW/SEND intent is temporary and usually not persistable: open() copes with that.
        open(uri, persist = false)
        return true
    }

    /** Makes [view] a drop target for video files (ChromeOS and Android multi-window drag-and-drop). */
    fun attachDropTarget(view: View) {
        view.setOnDragListener { _, event ->
            when (event.action) {
                DragEvent.ACTION_DRAG_STARTED -> event.clipData?.let { firstUri(it) != null } == true
                DragEvent.ACTION_DROP -> {
                    val uri = event.clipData?.let { firstUri(it) }
                    if (uri == null) {
                        false
                    } else {
                        // Without this the permission ends when the drop callback returns, before the worker reads.
                        ActivityCompat.requestDragAndDropPermissions(activity, event)
                        open(uri, persist = false)
                        true
                    }
                }
                else -> true
            }
        }
    }

    /** Stops the background thread; call from `Activity.onDestroy`. */
    fun shutdown() {
        worker.shutdown()
    }

    private fun firstUri(clip: ClipData): Uri? = (0 until clip.itemCount).firstNotNullOfOrNull { clip.getItemAt(it).uri }

    private fun open(
        uri: Uri,
        persist: Boolean,
    ) {
        if (persist) persistPermission(uri)
        worker.execute {
            val opened = openNow(uri)
            if (opened != null) activity.runOnUiThread { onOpened(opened) }
        }
    }

    private fun persistPermission(uri: Uri) {
        try {
            activity.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: SecurityException) {
            Log.w(TAG, "URI permission of $uri is not persistable: ${e.message}")
        }
    }

    private fun openNow(uri: Uri): OpenedFile? {
        val name = displayName(uri)
        val opened =
            try {
                activity.contentResolver.openFileDescriptor(uri, "r")
            } catch (e: java.io.IOException) {
                Log.e(TAG, "cannot open $uri: ${e.message}")
                null
            } catch (e: SecurityException) {
                Log.e(TAG, "no access to $uri: ${e.message}")
                null
            }
        val pfd = opened ?: return null
        // getStatSize() is -1 for a descriptor without a size; OvtNative.openFd treats that as "ask fstat".
        val size = pfd.statSize
        // detachFd() transfers ownership: from here the native side closes the descriptor, on failure too, and
        // the ParcelFileDescriptor must not be closed (that would close it twice).
        val session = OvtNative.openFd(pfd.detachFd(), size, name)
        return if (session != 0L) OpenedFile(session, name) else null
    }

    private fun displayName(uri: Uri): String {
        try {
            activity.contentResolver
                .query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { cursor ->
                    if (cursor.moveToFirst()) {
                        cursor.getString(0)?.let { return it }
                    }
                }
        } catch (e: RuntimeException) {
            Log.w(TAG, "no display name for $uri: ${e.message}")
        }
        return uri.lastPathSegment ?: "file"
    }

    companion object {
        private const val TAG = "OVT"

        /** MIME types offered to the document picker (§13.4.4 item 3). */
        val mimeTypes: List<String> = listOf("video/*", "application/octet-stream", "*/*")
    }
}
