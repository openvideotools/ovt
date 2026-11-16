// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.view.Surface

/**
 * Kotlin side of the JNI glue `android/app/src/main/cpp/ovt_jni.cpp` (chapter 13 §13.4.4). The native functions
 * are registered with RegisterNatives from JNI_OnLoad of libovt_jni.so; they are internal to the app and not part
 * of the C API of chapter 12. WI-0.050 added `ovt_jni_version`, WI-0.052 the surface, headroom and display entry
 * points; the file, lifecycle and plugin entry points arrive with WI-0.051 and WI-0.053.
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
     * `ovt_jni_surface_attach`: tags [surface] with the dataspace of the display's encoding, logs
     * `OVT_HDR dataspace=<BT2020_PQ|BT2020_HLG|SRGB>` and draws into it. Attaching the attached surface again only
     * redraws it. False when nothing could be drawn (the reason is in logcat, tag OVT).
     */
    @JvmStatic
    external fun surfaceAttach(
        session: Long,
        surface: Surface,
    ): Boolean

    /** `ovt_jni_surface_detach`: releases the surface; call from `surfaceDestroyed`. */
    @JvmStatic
    external fun surfaceDetach(session: Long)

    /** `ovt_jni_set_headroom`: the live `Display.hdrSdrRatio` (API 34+) for `DisplayTarget.headroom`. */
    @JvmStatic
    external fun setHeadroom(
        session: Long,
        hdrSdrRatio: Float,
    )

    /**
     * `ovt_jni_set_display_caps`: the display's HDR types and luminance (WI-4.458's `nativeSetDisplayCaps`
     * arguments), which pick the encoding of the next attach. See [DisplayCaps].
     */
    @JvmStatic
    external fun setDisplayCaps(
        session: Long,
        hdrTypes: IntArray,
        maxNits: Float,
        minNits: Float,
        maxFallNits: Float,
        hdrSdrRatio: Float,
        apiLevel: Int,
    )
}
