// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Context
import android.graphics.PixelFormat
import android.os.Build
import android.util.AttributeSet
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * The SurfaceView sibling below QtQuickView that carries HDR video (ADR-014, chapter 13 §13.4.4 item 2; never a
 * TextureView). WI-0.052: passes its Surface to `ovt_jni_surface_attach`, where native code sets the dataspace and
 * draws the PQ test pattern, and detaches it when the surface goes away. WI-A.400 adds the Picture-panel geometry
 * callbacks and the visibility rule; WI-A.401 replaces the pattern with libplacebo's output.
 *
 * Call [configure] before the view is attached to a window: the buffer format and the z-order are read when the
 * surface is created.
 */
class HdrSurfaceView
    @JvmOverloads
    constructor(
        context: Context,
        attrs: AttributeSet? = null,
    ) : SurfaceView(context, attrs),
        SurfaceHolder.Callback2 {
        /** Native session the surface belongs to; 0 until WI-0.051 brings sessions. */
        var session: Long = 0L

        init {
            // SPEC-ASSUMPTION(#436): below the Qt UI (§13.4.4 item 1, chapter 4 FR-AND-002): the SurfaceView default, the
            // setZOrderMediaOverlay(false) of §13.4.4 (deprecated in API 36). Never setZOrderOnTop, which would cover
            // the UI.
            holder.addCallback(this)
        }

        /**
         * Sets the buffer format for [caps] (RGBA_1010102 on an HDR display, RGBA_8888 otherwise), hands the display
         * figures to native code and asks the compositor for [desiredHeadroom] on API 35+ (§10.4.3;
         * SPEC-ASSUMPTION(#436): the API is 35, not 34).
         */
        fun configure(
            caps: DisplayCaps,
            desiredHeadroom: Float,
        ) {
            holder.setFormat(if (caps.hdrCapable) PixelFormat.RGBA_1010102 else PixelFormat.RGBA_8888)
            caps.pushToNative(session)
            if (caps.hdrCapable && Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
                setDesiredHdrHeadroom(desiredHeadroom)
            }
        }

        override fun surfaceCreated(holder: SurfaceHolder) {
            attach(holder)
        }

        override fun surfaceChanged(
            holder: SurfaceHolder,
            format: Int,
            width: Int,
            height: Int,
        ) {
            attach(holder) // same window: native code only redraws at the new size
        }

        override fun surfaceRedrawNeeded(holder: SurfaceHolder) {
            attach(holder)
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            OvtNative.surfaceDetach(session) // releases the window even when the attach failed
        }

        private fun attach(holder: SurfaceHolder) {
            val surface = holder.surface
            if (surface != null && surface.isValid) {
                OvtNative.surfaceAttach(session, surface) // false: logged natively, nothing drawn
            }
        }
    }
