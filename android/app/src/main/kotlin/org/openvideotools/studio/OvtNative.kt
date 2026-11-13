// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

/**
 * Kotlin side of the JNI glue `android/app/src/main/cpp/ovt_jni.cpp` (chapter 13 §13.4.4). The native functions
 * are registered with RegisterNatives from JNI_OnLoad of libovt_jni.so; they are internal to the app and not part
 * of the C API of chapter 12. WI-0.050 has `ovt_jni_version` only; the file, surface, headroom, lifecycle and
 * plugin entry points arrive with WI-0.051, WI-0.052 and WI-0.053.
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
}
