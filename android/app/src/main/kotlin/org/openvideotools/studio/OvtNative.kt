// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

/**
 * Kotlin side of the JNI glue `android/app/src/main/cpp/ovt_jni.cpp` (chapter 13 §13.4.4). The native functions
 * are registered with RegisterNatives from JNI_OnLoad of libovt_jni.so; they are internal to the app and not part
 * of the C API of chapter 12. WI-0.050 has `ovt_jni_version`, WI-0.051 adds the file entry points; the surface,
 * headroom, lifecycle and plugin entry points arrive with WI-0.052 and WI-0.053.
 */
object OvtNative {
    init {
        System.loadLibrary("ovt_jni")
    }

    /**
     * `ovt_jni_version`: `{"version":"<ovt_version_string()>","build":<ovt_build_info_json()>}`, or null if the
     * native side could not allocate the string.
     */
    @JvmStatic
    external fun version(): String?

    /**
     * `ovt_jni_open_fd`: [fd] must come from `ParcelFileDescriptor.detachFd()`; the native side takes ownership at
     * the call and closes it on failure too, so the caller never closes it. [size] is
     * `ParcelFileDescriptor.getStatSize()` (a value <= 0 means unknown: the library asks fstat). Returns a session
     * handle, or 0 when the file or its container could not be opened (logged under tag `OVT`).
     */
    @JvmStatic
    external fun openFd(
        fd: Int,
        size: Long,
        displayName: String?,
    ): Long

    /** `ovt_jni_close`: closes the container, the file and its descriptor. 0 is a no-op. */
    @JvmStatic
    external fun close(session: Long)

    /** `ovt_jni_file_size`: `ovt_file_size()` of the session's file, 64-bit; 0 for no session. */
    @JvmStatic
    external fun fileSize(session: Long): Long

    /** `ovt_jni_info_json`: the debug overlay's JSON (file, container format, tracks), or null. */
    @JvmStatic
    external fun infoJson(session: Long): String?
}
