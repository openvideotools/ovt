// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

/**
 * Storage Access Framework file picking (chapter 13 §13.4.4 item 3): ACTION_OPEN_DOCUMENT, persisted URI
 * permissions and ParcelFileDescriptor.detachFd() into ovt_file_open_fd. Stub of WI-0.050; WI-0.051 implements it.
 */
internal object FilePicker {
    /** MIME types offered to the document picker (§13.4.4 item 3). */
    val mimeTypes: List<String> = listOf("video/*", "application/octet-stream", "*/*")
}
