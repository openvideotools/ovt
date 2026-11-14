// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.pm.ActivityInfo
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Display
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import org.qtproject.qt.android.QtQuickView
import java.util.function.Consumer

/**
 * The single Activity of OVT Studio (chapter 13 §13.4.4; WI-0.050). A plain Android Activity that embeds Qt Quick
 * through [QtQuickView] (ADR-004), not Qt's own QtActivity, so that it can own the HDR SurfaceView sibling
 * (ADR-014). WI-0.050 sets up the layout of item 1 and the JNI library, WI-0.052 the HDR output of item 2; the later
 * items arrive with WI-0.051 (files), WI-0.053 (plugins) and WI-0.054 (ChromeOS configuration changes).
 */
class MainActivity : AppCompatActivity() {
    private lateinit var hdrSurface: HdrSurfaceView
    private lateinit var qtView: QtQuickView
    private val headroomListener =
        Consumer<Display> { changed -> OvtNative.setHeadroom(hdrSurface.session, changed.hdrSdrRatio) }
    private var headroomDisplay: Display? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val host = findViewById<FrameLayout>(R.id.ovt_host)

        // Item 2: what the display can show decides the encoding. COLOR_MODE_HDR before the surface exists (§10.4.3).
        val caps = DisplayCaps.query(currentDisplay())
        Log.i(TAG, "OVT: $caps")
        if (caps.hdrCapable) {
            window.colorMode = ActivityInfo.COLOR_MODE_HDR
        }

        // Bottom child: the HDR picture surface with the PQ test pattern of WI-0.052. Until WI-A.400 follows the
        // Picture panel's geometry it fills the area below the header band of AndroidShell.qml.
        hdrSurface = HdrSurfaceView(this)
        hdrSurface.configure(caps, PATTERN_HEADROOM)
        val surfaceParams =
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        host.addView(hdrSurface, surfaceParams)
        host.addOnLayoutChangeListener { view, _, _, _, _, _, _, _, _ ->
            val top = (view.height * SHELL_HEADER_FRACTION).toInt()
            if (surfaceParams.topMargin != top) {
                surfaceParams.topMargin = top
                view.post { hdrSurface.layoutParams = surfaceParams } // not inside the layout pass
            }
        }

        // Top child: the Qt Quick UI. Constructing QtQuickView loads the Qt libraries and libovt_studio_<abi>.so,
        // runs its main() (which only starts the event loop on Android) and then loads the QML root below.
        qtView = QtQuickView(this, SHELL_QML, QT_APP_LIBRARY)
        host.addView(
            qtView,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )

        Log.i(TAG, "OVT: native ${OvtNative.version()}")
    }

    override fun onStart() {
        super.onStart()
        // §10.4.3: the live headroom (API 34+) for DisplayTarget.headroom.
        val current = currentDisplay()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && current.isHdrSdrRatioAvailable) {
            current.registerHdrSdrRatioChangedListener(mainExecutor, headroomListener)
            headroomDisplay = current
            OvtNative.setHeadroom(hdrSurface.session, current.hdrSdrRatio)
        }
    }

    override fun onStop() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            headroomDisplay?.unregisterHdrSdrRatioChangedListener(headroomListener)
        }
        headroomDisplay = null
        super.onStop()
    }

    /** The display the Activity is on (Context.getDisplay needs API 30; minSdk is 28). */
    @Suppress("DEPRECATION")
    private fun currentDisplay(): Display =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display ?: windowManager.defaultDisplay
        } else {
            windowManager.defaultDisplay
        }

    private companion object {
        const val TAG = "OVT"

        /** Share of the window height that AndroidShell.qml's header band covers (keep both in step). */
        const val SHELL_HEADER_FRACTION = 0.25f

        /**
         * Headroom asked of the compositor for the test pattern (API 35+): the full-screen picture value of §10.4.3,
         * so that the 1000-nit patch can reach its level. WI-A.404 switches between 2.0 and 8.0 with the layout.
         */
        const val PATTERN_HEADROOM = 8.0f

        /** The Qt application library: Qt appends `_<abi>` (libovt_studio_x86_64.so, §13.2.1). */
        const val QT_APP_LIBRARY = "ovt_studio"

        /** QML root for QtQuickView, which needs an Item, not the desktop ApplicationWindow (app/qml/AndroidShell.qml). */
        const val SHELL_QML = "qrc:/qt/qml/OvtStudio/AndroidShell.qml"
    }
}
