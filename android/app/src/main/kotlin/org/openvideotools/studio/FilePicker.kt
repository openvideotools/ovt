// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.ClipData
import android.content.ContentResolver
import android.content.Intent
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import android.view.DragEvent
import android.view.View
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.VisibleForTesting
import androidx.annotation.WorkerThread
import androidx.core.app.ActivityCompat
import androidx.core.content.IntentCompat
import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.util.concurrent.Executors

/**
 * Storage Access Framework file opening (chapter 13 §13.4.4 item 3; WI-0.051). No storage permission is used: a
 * document comes from `ActivityResultContracts.OpenDocument()`, an `ACTION_VIEW`/`ACTION_SEND` intent, a
 * drag-and-drop or the recent list, its URI permission is persisted where the provider allows it (recents), and
 * `ContentResolver.openFileDescriptor` → `ParcelFileDescriptor.detachFd()` hands the descriptor to
 * `ovt_file_open_fd` through [OvtNative.openFd], which owns it from then on.
 *
 * WI-A.452 (chapter 11 §11.5.6) adds the recent list ([RecentFiles]; a picked document's grant is taken through
 * [PersistedGrants]) and the seekability check: the core reads with `pread`, so a descriptor that `fstat` does not
 * report as a regular file (a pipe or socket from a streaming or cloud provider) is not handed over. [onNotSeekable]
 * gets it instead, and MainActivity raises `GUI-AND-0001` with the copy action ([CopyToAppStorage]). Nothing is read
 * from the document to decide this.
 *
 * Construct it in `Activity.onCreate` (the result launcher must be registered before the Activity starts). The
 * open itself runs on a single background thread because a provider can be slow; [onOpened] and [onNotSeekable] are
 * called on the main thread. [onOpened] gets the new session handle and replaces (closes) the previous session.
 */
internal class FilePicker(
    private val activity: ComponentActivity,
    private val recentFiles: RecentFiles,
    private val onOpened: (OpenedFile) -> Unit,
    private val onNotSeekable: (uri: Uri, displayName: String) -> Unit,
) {
    /** A file opened in the native core. [session] is the `ovt_jni_open_fd` handle. */
    class OpenedFile(
        val session: Long,
        val displayName: String,
        val uri: Uri,
    )

    /** What [openDocument] found. */
    sealed interface OpenResult {
        /** Opened; [codec] is the recent list's codec field (SPEC-ASSUMPTION(#723): the container format). */
        class Opened(
            val file: OpenedFile,
            val codec: String,
        ) : OpenResult

        /** `fstat` says the descriptor is not a regular file: `GUI-AND-0001`. */
        class NotSeekable(
            val uri: Uri,
            val displayName: String,
        ) : OpenResult

        /** Not opened. [gone] when the document no longer exists or is no longer readable by the app. */
        class Failed(
            val gone: Boolean,
        ) : OpenResult
    }

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

    /**
     * Opens a recent list entry again. Its grant was persisted when it was picked; if the document is gone or the
     * grant was evicted, the entry is removed from the list.
     */
    fun reopen(entry: RecentFiles.Entry) {
        open(entry.uri, persist = false, name = entry.displayName, dropIfGone = true)
    }

    /** Opens the local copy that [CopyToAppStorage] made of the document [displayName]. */
    fun openCopy(
        file: File,
        displayName: String,
    ) {
        open(Uri.fromFile(file), persist = false, name = displayName)
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

    /**
     * Opens [uri] on the worker thread. [persist] takes a persistable grant first (documents from the picker);
     * [name] replaces the provider's display name; [dropIfGone] removes the recent entry of a document that is gone.
     */
    @VisibleForTesting
    internal fun open(
        uri: Uri,
        persist: Boolean,
        name: String? = null,
        dropIfGone: Boolean = false,
    ) {
        if (persist) PersistedGrants.take(activity.contentResolver, uri, recentFiles.uris())
        worker.execute {
            val result = openDocument(activity.contentResolver, uri, name)
            activity.runOnUiThread { deliver(uri, result, dropIfGone) }
        }
    }

    private fun deliver(
        uri: Uri,
        result: OpenResult,
        dropIfGone: Boolean,
    ) {
        when (result) {
            is OpenResult.Opened -> {
                recentFiles.add(uri, result.file.displayName, result.codec)
                onOpened(result.file)
            }
            is OpenResult.NotSeekable -> onNotSeekable(result.uri, result.displayName)
            is OpenResult.Failed -> if (dropIfGone && result.gone) recentFiles.remove(uri)
        }
    }

    companion object {
        private const val TAG = "OVT"

        /** MIME types offered to the document picker (§13.4.4 item 3). */
        val mimeTypes: List<String> = listOf("video/*", "application/octet-stream", "*/*")

        /**
         * Opens [uri] in the native core (blocking: call it off the main thread). A descriptor that is not a regular
         * file is closed again and reported as [OpenResult.NotSeekable]; only `fstat` decides, the document is not
         * read.
         */
        @WorkerThread
        fun openDocument(
            resolver: ContentResolver,
            uri: Uri,
            name: String? = null,
        ): OpenResult {
            val displayName = name ?: displayName(resolver, uri)
            val pfd =
                try {
                    resolver.openFileDescriptor(uri, "r")
                } catch (e: FileNotFoundException) {
                    Log.e(TAG, "cannot open $uri: ${e.message}")
                    return OpenResult.Failed(gone = true)
                } catch (e: IOException) {
                    Log.e(TAG, "cannot open $uri: ${e.message}")
                    return OpenResult.Failed(gone = false)
                } catch (e: SecurityException) {
                    Log.e(TAG, "no access to $uri: ${e.message}")
                    return OpenResult.Failed(gone = true)
                } catch (e: RuntimeException) {
                    // Providers also throw IllegalArgumentException/IllegalStateException for documents they reject.
                    Log.e(TAG, "provider failed to open $uri: $e")
                    return OpenResult.Failed(gone = false)
                } ?: return OpenResult.Failed(gone = false)
            if (!isRegularFile(pfd)) {
                pfd.close()
                Log.e(TAG, "$GUI_AND_0001: $GUI_AND_0001_MESSAGE ($displayName)")
                return OpenResult.NotSeekable(uri, displayName)
            }
            // getStatSize() is -1 for a descriptor without a size; OvtNative.openFd treats that as "ask fstat".
            val size = pfd.statSize
            // detachFd() transfers ownership: from here the native side closes the descriptor, on failure too, and
            // the ParcelFileDescriptor must not be closed (that would close it twice).
            val session = OvtNative.openFd(pfd.detachFd(), size, displayName)
            if (session == 0L) return OpenResult.Failed(gone = false)
            return OpenResult.Opened(OpenedFile(session, displayName, uri), containerFormat(session))
        }

        /** `GUI-AND-0001` of the §11.7 registry. */
        const val GUI_AND_0001 = "GUI-AND-0001"
        const val GUI_AND_0001_MESSAGE = "provider does not support seeking; copy the file locally"

        /** True when `fstat` reports a regular file, the only kind `pread` can serve (§11.7: "fstat not regular"). */
        private fun isRegularFile(pfd: ParcelFileDescriptor): Boolean =
            try {
                OsConstants.S_ISREG(Os.fstat(pfd.fileDescriptor).st_mode)
            } catch (e: ErrnoException) {
                Log.w(TAG, "fstat failed: ${e.message}")
                false
            }

        private fun containerFormat(session: Long): String =
            try {
                OvtNative.infoJson(session)?.let { JSONObject(it).optString("format") } ?: ""
            } catch (e: JSONException) {
                ""
            }

        private fun displayName(
            resolver: ContentResolver,
            uri: Uri,
        ): String {
            try {
                resolver
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
    }
}
