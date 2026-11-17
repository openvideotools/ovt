// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import android.util.Log
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException

/**
 * The test `DocumentsProvider` of SafFlowsTest (WI-A.452; debug builds only, disabled until a test enables it). It
 * serves the files of `cacheDir/saf-test/` as one flat root. A document whose name starts with [PIPE_PREFIX] is
 * served as the read end of a pipe, the way streaming and cloud providers hand out documents they cannot seek in, so
 * the app must raise `GUI-AND-0001`; every other document is the file itself and is seekable.
 */
class TestDocumentsProvider : DocumentsProvider() {
    override fun onCreate(): Boolean = true

    override fun queryRoots(projection: Array<out String>?): Cursor =
        MatrixCursor(projection ?: ROOT_COLUMNS).apply {
            newRow()
                .add(Root.COLUMN_ROOT_ID, ROOT_ID)
                .add(Root.COLUMN_DOCUMENT_ID, ROOT_DOCUMENT_ID)
                .add(Root.COLUMN_TITLE, "OVT test documents")
                .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE or Root.FLAG_LOCAL_ONLY)
                .add(Root.COLUMN_ICON, android.R.drawable.ic_menu_gallery)
        }

    override fun queryDocument(
        documentId: String,
        projection: Array<out String>?,
    ): Cursor = MatrixCursor(projection ?: DOCUMENT_COLUMNS).apply { addDocument(this, documentId) }

    override fun queryChildDocuments(
        parentDocumentId: String,
        projection: Array<out String>?,
        sortOrder: String?,
    ): Cursor =
        MatrixCursor(projection ?: DOCUMENT_COLUMNS).apply {
            dir().listFiles()?.sortedBy { it.name }?.forEach { addDocument(this, it.name) }
        }

    override fun openDocument(
        documentId: String,
        mode: String,
        signal: CancellationSignal?,
    ): ParcelFileDescriptor {
        val file = fileOf(documentId)
        if (documentId.startsWith(PIPE_PREFIX) && mode == "r") return pipeOf(file)
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode))
    }

    override fun createDocument(
        parentDocumentId: String,
        mimeType: String,
        displayName: String,
    ): String {
        val file = File(dir(), displayName)
        if (!file.createNewFile()) throw FileNotFoundException("$displayName exists")
        return displayName
    }

    private fun addDocument(
        cursor: MatrixCursor,
        documentId: String,
    ) {
        val row = cursor.newRow().add(Document.COLUMN_DOCUMENT_ID, documentId)
        if (documentId == ROOT_DOCUMENT_ID) {
            row
                .add(Document.COLUMN_DISPLAY_NAME, "OVT test documents")
                .add(Document.COLUMN_MIME_TYPE, Document.MIME_TYPE_DIR)
                .add(Document.COLUMN_FLAGS, Document.FLAG_DIR_SUPPORTS_CREATE)
            return
        }
        val file = fileOf(documentId)
        row
            .add(Document.COLUMN_DISPLAY_NAME, documentId)
            .add(Document.COLUMN_MIME_TYPE, "application/octet-stream")
            .add(Document.COLUMN_SIZE, file.length())
            .add(Document.COLUMN_LAST_MODIFIED, file.lastModified())
            .add(Document.COLUMN_FLAGS, Document.FLAG_SUPPORTS_WRITE)
    }

    private fun dir(): File = File(requireNotNull(context).cacheDir, DIR_NAME).apply { mkdirs() }

    private fun fileOf(documentId: String): File {
        val file = File(dir(), documentId)
        if (file.parentFile != dir() || !file.exists()) throw FileNotFoundException(documentId)
        return file
    }

    // A pipe: the reader gets the bytes in order and cannot seek, and fstat reports S_IFIFO.
    private fun pipeOf(file: File): ParcelFileDescriptor {
        val (readEnd, writeEnd) = ParcelFileDescriptor.createPipe()
        Thread({
            try {
                ParcelFileDescriptor.AutoCloseOutputStream(writeEnd).use { out ->
                    file.inputStream().use { it.copyTo(out) }
                }
            } catch (e: IOException) {
                // The reader closed its end early (the app's fstat check does exactly that).
                Log.i("OVT", "test pipe of ${file.name} closed: ${e.message}")
            }
        }, "ovt-test-pipe").start()
        return readEnd
    }

    companion object {
        /** Documents named so are served through a pipe (not seekable). */
        const val PIPE_PREFIX = "pipe-"

        /** The directory under `cacheDir` that holds the documents. */
        const val DIR_NAME = "saf-test"
        const val ROOT_DOCUMENT_ID = "root"
        private const val ROOT_ID = "ovt-test"

        private val ROOT_COLUMNS =
            arrayOf(Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE, Root.COLUMN_FLAGS, Root.COLUMN_ICON)
        private val DOCUMENT_COLUMNS =
            arrayOf(
                Document.COLUMN_DOCUMENT_ID,
                Document.COLUMN_DISPLAY_NAME,
                Document.COLUMN_MIME_TYPE,
                Document.COLUMN_SIZE,
                Document.COLUMN_LAST_MODIFIED,
                Document.COLUMN_FLAGS,
            )

        /** The provider's authority in the app [packageName] (the manifest's `${applicationId}.testdocuments`). */
        fun authority(packageName: String): String = "$packageName.testdocuments"
    }
}
