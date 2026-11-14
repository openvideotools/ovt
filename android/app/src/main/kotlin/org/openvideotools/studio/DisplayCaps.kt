// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.os.Build
import android.view.Display

/**
 * What the display the Activity is on can show (chapter 13 §13.4.4 item 2, chapter 10 §10.4.3). Stub of WI-0.052:
 * the HDR types and luminance figures that decide the output encoding, passed to native code before the surface is
 * attached, and a one-line diagnostic. WI-A.404 adds the display listener and the layout-dependent headroom;
 * WI-4.458 moves the native side into libovt-render.
 */
class DisplayCaps private constructor(
    /** `Display.HdrCapabilities.HDR_TYPE_*`; empty on an SDR panel (Pixel Tablet, emulator). */
    val hdrTypes: IntArray,
    val maxNits: Float,
    val minNits: Float,
    val maxFallNits: Float,
    /** `Display.hdrSdrRatio` on API 34+ when available, else 1. */
    val hdrSdrRatio: Float,
) {
    /** True when the §10.4.3 encoding rule picks PQ or HLG. */
    val hdrCapable: Boolean
        get() = hdrTypes.any { it == HDR_TYPE_HDR10 || it == HDR_TYPE_HDR10_PLUS || it == HDR_TYPE_HLG }

    /** Hands the figures to `ovt_jni_set_display_caps`; call before the surface is attached. */
    fun pushToNative(session: Long = 0L) {
        OvtNative.setDisplayCaps(session, hdrTypes, maxNits, minNits, maxFallNits, hdrSdrRatio, Build.VERSION.SDK_INT)
    }

    override fun toString(): String =
        "DisplayCaps(hdrTypes=${hdrTypes.joinToString(",", "[", "]")}, maxNits=$maxNits, minNits=$minNits, " +
            "maxFallNits=$maxFallNits, hdrSdrRatio=$hdrSdrRatio, hdrCapable=$hdrCapable)"

    companion object {
        // Display.HdrCapabilities constants (API 24), spelt out so that the rule reads like §10.4.3.
        const val HDR_TYPE_DOLBY_VISION = 1
        const val HDR_TYPE_HDR10 = 2
        const val HDR_TYPE_HLG = 3
        const val HDR_TYPE_HDR10_PLUS = 4

        /**
         * Queries [display]. HDR types come from `Display.Mode.getSupportedHdrTypes()` on API 34+ and from the
         * deprecated `Display.getHdrCapabilities()` below; luminance only exists in the latter. A display that does
         * not report `isHdr` gets no types, so that it stays SDR (WI-0.052 acceptance test 2).
         */
        fun query(display: Display): DisplayCaps {
            @Suppress("DEPRECATION")
            val capabilities = display.hdrCapabilities
            val types =
                when {
                    !display.isHdr -> IntArray(0)
                    Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE -> display.mode.supportedHdrTypes
                    else ->
                        @Suppress("DEPRECATION")
                        capabilities?.supportedHdrTypes ?: IntArray(0)
                }
            val ratio =
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && display.isHdrSdrRatioAvailable) {
                    display.hdrSdrRatio
                } else {
                    1.0f
                }
            return DisplayCaps(
                hdrTypes = types,
                maxNits = capabilities?.desiredMaxLuminance ?: 0.0f,
                minNits = capabilities?.desiredMinLuminance ?: 0.0f,
                maxFallNits = capabilities?.desiredMaxAverageLuminance ?: 0.0f,
                hdrSdrRatio = ratio,
            )
        }
    }
}
