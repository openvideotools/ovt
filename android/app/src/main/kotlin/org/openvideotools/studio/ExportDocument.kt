// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.app.Activity
import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContract
import androidx.annotation.MainThread
import androidx.annotation.VisibleForTesting
import androidx.annotation.WorkerThread
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.FileNotFoundException
import java.io.IOException
import java.io.OutputStream
import java.util.concurrent.Executors

/**
 * Export through the Storage Access Framework (chapter 11 §11.5.6; WI-A.452): `ACTION_CREATE_DOCUMENT` lets the user
 * choose where the file goes (any provider: Downloads, a cloud drive, a USB stick), and the request's writer fills the
 * new document on a background thread. No storage permission is used.
 *
 * FR-AND-017: a PNG export of HDR content is SDR tone-mapped on Android, and the user is told so before the picker
 * opens ([needsSdrNotice]).
 *
 * Construct it in `Activity.onCreate` (the result launcher must be registered before the Activity starts). One export
 * runs at a time; a request made while the picker is open replaces the pending one.
 */
internal class ExportDocument(
    private val activity: AppCompatActivity,
) {
    /**
     * One export. [writer] runs on a background thread with the new document's stream and must not close it; an
     * exception from it fails the export.
     */
    class Request(
        val suggestedName: String,
        val mimeType: String,
        val hdrContent: Boolean = false,
        val writer: (OutputStream) -> Unit,
    )

    private var pending: Request? = null
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "ovt-export").apply { isDaemon = true } }
    private val launcher =
        activity.registerForActivityResult(CreateDocument()) { uri ->
            val request = pending
            pending = null
            if (uri == null || request == null) return@registerForActivityResult
            worker.execute {
                val ok = write(activity.contentResolver, uri, request.writer)
                activity.runOnUiThread {
                    if (!activity.isDestroyed) {
                        val text = if (ok) "Exported ${request.suggestedName}" else "Export of ${request.suggestedName} failed"
                        Toast.makeText(activity, text, Toast.LENGTH_SHORT).show()
                    }
                }
            }
        }

    /** Starts [request]: the FR-AND-017 notice when it applies, then the system "save as" picker. */
    @MainThread
    fun export(request: Request) {
        if (needsSdrNotice(request.mimeType, request.hdrContent)) {
            AlertDialog
                .Builder(activity)
                .setTitle(SDR_NOTICE_TITLE)
                .setMessage(SDR_NOTICE)
                .setPositiveButton(android.R.string.ok) { _, _ -> launch(request) }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        } else {
            launch(request)
        }
    }

    /** Stops the background thread; call from `Activity.onDestroy`. A write in progress finishes. */
    fun shutdown() {
        worker.shutdown()
    }

    private fun launch(request: Request) {
        pending = request
        launcher.launch(request)
    }

    /** `ACTION_CREATE_DOCUMENT` with the request's MIME type and suggested name (`CATEGORY_OPENABLE`). */
    @VisibleForTesting
    internal class CreateDocument : ActivityResultContract<Request, Uri?>() {
        override fun createIntent(
            context: Context,
            input: Request,
        ): Intent =
            Intent(Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType(input.mimeType)
                .putExtra(Intent.EXTRA_TITLE, input.suggestedName)

        override fun parseResult(
            resultCode: Int,
            intent: Intent?,
        ): Uri? = if (resultCode == Activity.RESULT_OK) intent?.data else null
    }

    companion object {
        private const val TAG = "OVT"
        private const val SDR_NOTICE_TITLE = "SDR export"

        /** FR-AND-017's statement, with the HDR-preserving alternative of FR-PLN-010. */
        const val SDR_NOTICE =
            "This picture is HDR. On Android a PNG export is tone-mapped to SDR. To keep the HDR values, export a " +
                "16-bit PNG or the raw planes instead."

        /** FR-AND-017: an HDR picture exported as PNG is SDR tone-mapped, and the user is told before choosing. */
        fun needsSdrNotice(
            mimeType: String,
            hdrContent: Boolean,
        ): Boolean = hdrContent && mimeType.equals("image/png", ignoreCase = true)

        /**
         * Writes [writer]'s output into the document [uri] (truncating it: the user may have picked an existing
         * file). Returns false, after logging, when the document cannot be opened or the writer throws.
         */
        @WorkerThread
        fun write(
            resolver: ContentResolver,
            uri: Uri,
            writer: (OutputStream) -> Unit,
        ): Boolean =
            try {
                val stream = openTruncated(resolver, uri) ?: throw IOException("provider returned no stream")
                stream.use { writer(it) }
                Log.i(TAG, "exported to $uri")
                true
            } catch (e: IOException) {
                Log.e(TAG, "export to $uri failed: ${e.message}")
                false
            } catch (e: RuntimeException) {
                // SecurityException (the grant is gone) or a failure inside the writer.
                Log.e(TAG, "export to $uri failed: $e")
                false
            }

        // "wt" truncates; providers that do not know the "t" flag reject it, and then a plain "w" is the best left.
        private fun openTruncated(
            resolver: ContentResolver,
            uri: Uri,
        ): OutputStream? =
            try {
                resolver.openOutputStream(uri, "wt")
            } catch (e: FileNotFoundException) {
                resolver.openOutputStream(uri, "w")
            } catch (e: IllegalArgumentException) {
                resolver.openOutputStream(uri, "w")
            }
    }
}
