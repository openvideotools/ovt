// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Context
import android.content.DialogInterface
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Log
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.annotation.MainThread
import androidx.annotation.VisibleForTesting
import androidx.annotation.WorkerThread
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.IOException
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The "Copy to app storage" action of `GUI-AND-0001` (chapter 11 §11.5.6, §11.7; WI-A.452; FR-AND-005): a document
 * whose provider hands out a pipe instead of a seekable file is streamed once into app-specific storage, and the
 * local copy is opened instead.
 *
 * SPEC-ASSUMPTION(#722): the copy goes to `getExternalFilesDir(null)/copies/` as §11.5.6 says, not to the cache
 * directory of FR-AND-005, because the system deletes cache files on its own and the copy is what the recent list (and
 * a project) points at. Without external storage it falls back to `filesDir/copies/`.
 */
internal class CopyToAppStorage(
    private val context: Context,
) {
    /**
     * Streams [uri] into [destinationDir] under a file name made from [displayName] (never replacing an existing
     * file) and returns the new file. [progress] gets (bytes copied, total bytes or -1 when the provider does not say)
     * after every block; [cancelled] is polled between blocks and stops the copy with [CancelledException]. A failed or
     * cancelled copy leaves no file behind.
     */
    @WorkerThread
    fun copy(
        uri: Uri,
        displayName: String,
        cancelled: () -> Boolean = { false },
        progress: (copied: Long, total: Long) -> Unit = { _, _ -> },
    ): File {
        val resolver = context.contentResolver
        val total = documentSize(uri)
        val dir = destinationDir(context).apply { mkdirs() }
        val target = uniqueFile(dir, safeFileName(displayName))
        val partial = File(dir, target.name + PARTIAL_SUFFIX)
        try {
            val input = resolver.openInputStream(uri) ?: throw IOException("provider returned no stream for $uri")
            input.use { source ->
                partial.outputStream().use { sink ->
                    val buffer = ByteArray(BUFFER_BYTES)
                    var copied = 0L
                    while (true) {
                        if (cancelled()) throw CancelledException()
                        val read = source.read(buffer)
                        if (read < 0) break
                        sink.write(buffer, 0, read)
                        copied += read
                        progress(copied, total)
                    }
                    sink.fd.sync()
                }
            }
            if (!partial.renameTo(target)) throw IOException("cannot rename $partial to $target")
            Log.i(TAG, "copied $displayName to $target (${target.length()} bytes)")
            return target
        } catch (e: IOException) {
            partial.delete()
            throw e
        } catch (e: SecurityException) {
            partial.delete()
            throw IOException("no access to $uri: ${e.message}", e)
        }
    }

    /** Thrown by [copy] when its `cancelled` callback said so. */
    class CancelledException : IOException("copy cancelled")

    private fun documentSize(uri: Uri): Long {
        try {
            context.contentResolver
                .query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)
                ?.use { cursor ->
                    if (cursor.moveToFirst() && !cursor.isNull(0)) return cursor.getLong(0)
                }
        } catch (e: RuntimeException) {
            Log.w(TAG, "no size for $uri: ${e.message}")
        }
        return -1L
    }

    companion object {
        private const val TAG = "OVT"
        private const val BUFFER_BYTES = 1 shl 20
        private const val PARTIAL_SUFFIX = ".part"
        private const val MAX_NAME_CHARS = 120

        /** Where copies go: `getExternalFilesDir(null)/copies`, or `filesDir/copies` without external storage. */
        fun destinationDir(context: Context): File = File(context.getExternalFilesDir(null) ?: context.filesDir, "copies")

        /** [displayName] as a single safe file name: no path separators, control characters or leading dots. */
        @VisibleForTesting
        internal fun safeFileName(displayName: String): String {
            val cleaned =
                displayName
                    .map { c -> if (c == '/' || c == '\\' || c == ':' || c.isISOControl()) '_' else c }
                    .joinToString("")
                    .trim()
                    .trimStart('.')
                    .take(MAX_NAME_CHARS)
            return cleaned.ifEmpty { "document" }
        }

        /** [name] in [dir], or `stem (n).ext` with the first free n. */
        @VisibleForTesting
        internal fun uniqueFile(
            dir: File,
            name: String,
        ): File {
            val first = File(dir, name)
            if (!first.exists()) return first
            val dot = name.lastIndexOf('.').takeIf { it > 0 } ?: name.length
            val stem = name.substring(0, dot)
            val extension = name.substring(dot)
            return generateSequence(1) { it + 1 }
                .map { File(dir, "$stem ($it)$extension") }
                .first { !it.exists() }
        }
    }
}

/**
 * The `GUI-AND-0001` error dialog with its "Copy to app storage" action, and the copy's progress dialog
 * (FR-AND-005: a visible progress indicator). [onCopied] gets the local file and the document's display name on the
 * main thread. Owned by MainActivity; [cancel] stops a running copy when the Activity goes away.
 */
internal class CopyPrompt(
    private val activity: AppCompatActivity,
    private val copier: CopyToAppStorage,
    private val onCopied: (File, String) -> Unit,
) {
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "ovt-copy").apply { isDaemon = true } }
    private val cancelled = AtomicBoolean(false)

    /** The dialog on screen (the error or the progress), for tests. */
    @VisibleForTesting
    internal var dialog: AlertDialog? = null
        private set

    /** Raises `GUI-AND-0001` for the document [uri] named [displayName], offering the copy. */
    @MainThread
    fun show(
        uri: Uri,
        displayName: String,
    ) {
        dialog?.dismiss()
        dialog =
            AlertDialog
                .Builder(activity)
                .setTitle(FilePicker.GUI_AND_0001)
                .setMessage("${FilePicker.GUI_AND_0001_MESSAGE}\n\n$displayName")
                .setPositiveButton(COPY_LABEL) { _, _ -> startCopy(uri, displayName) }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
    }

    /** Stops a running copy and closes the dialog (Activity.onDestroy). */
    @MainThread
    fun cancel() {
        cancelled.set(true)
        dialog?.dismiss()
        dialog = null
        worker.shutdownNow()
    }

    private fun startCopy(
        uri: Uri,
        displayName: String,
    ) {
        cancelled.set(false)
        val bar = ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal).apply { isIndeterminate = true }
        val label = TextView(activity).apply { text = displayName }
        val content =
            LinearLayout(activity).apply {
                orientation = LinearLayout.VERTICAL
                val pad = (PAD_DP * resources.displayMetrics.density).toInt()
                setPadding(pad, pad, pad, pad)
                addView(label)
                addView(bar)
            }
        val progressDialog =
            AlertDialog
                .Builder(activity)
                .setTitle(COPYING_TITLE)
                .setView(content)
                .setCancelable(false)
                .setNegativeButton(android.R.string.cancel) { _: DialogInterface, _: Int -> cancelled.set(true) }
                .show()
        dialog = progressDialog
        var lastPercent = -1
        worker.execute {
            val result =
                runCatching {
                    copier.copy(uri, displayName, cancelled::get) { copied, total ->
                        if (total > 0) {
                            val percent = (copied * PERCENT / total).toInt().coerceIn(0, PERCENT)
                            if (percent != lastPercent) {
                                lastPercent = percent
                                activity.runOnUiThread {
                                    bar.isIndeterminate = false
                                    bar.max = PERCENT
                                    bar.progress = percent
                                }
                            }
                        }
                    }
                }
            activity.runOnUiThread {
                progressDialog.dismiss()
                if (dialog === progressDialog) dialog = null
                result
                    .onSuccess { file -> onCopied(file, displayName) }
                    .onFailure { error ->
                        if (error !is CopyToAppStorage.CancelledException && !activity.isDestroyed) {
                            Log.e(TAG, "copy of $displayName failed: ${error.message}")
                            Toast.makeText(activity, "$COPY_FAILED ${error.message}", Toast.LENGTH_LONG).show()
                        }
                    }
            }
        }
    }

    companion object {
        private const val TAG = "OVT"
        private const val PERCENT = 100
        private const val PAD_DP = 24

        /** The action's label (§11.5.6). */
        const val COPY_LABEL = "Copy to app storage"
        private const val COPYING_TITLE = "Copying to app storage…"
        private const val COPY_FAILED = "The copy failed:"
    }
}
