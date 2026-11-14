// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.os.Bundle
import android.util.Log
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import org.qtproject.qt.android.QtQuickView

/**
 * The single Activity of OVT Studio (chapter 13 §13.4.4; WI-0.050). A plain Android Activity that embeds Qt Quick
 * through [QtQuickView] (ADR-004), not Qt's own QtActivity, so that it can own the HDR SurfaceView sibling
 * (ADR-014). WI-0.050 sets up the layout of item 1 and the JNI library; the later items arrive with WI-0.051
 * (files), WI-0.052 (HDR output), WI-0.053 (plugins) and WI-0.054 (ChromeOS configuration changes).
 */
class MainActivity : AppCompatActivity() {
    private lateinit var hdrSurface: HdrSurfaceView
    private lateinit var qtView: QtQuickView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val host = findViewById<FrameLayout>(R.id.ovt_host)

        // Bottom child: the HDR picture surface, positioned by the Picture panel's geometry from WI-0.052 on.
        hdrSurface = HdrSurfaceView(this)
        host.addView(hdrSurface, FrameLayout.LayoutParams(0, 0))

        // Top child: the Qt Quick UI. Constructing QtQuickView loads the Qt libraries and libovt_studio_<abi>.so,
        // runs its main() (which only starts the event loop on Android) and then loads the QML root below.
        qtView = QtQuickView(this, SHELL_QML, QT_APP_LIBRARY)
        host.addView(
            qtView,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )

        Log.i(TAG, "OVT: native ${OvtNative.version()}")
    }

    private companion object {
        const val TAG = "OVT"

        /** The Qt application library: Qt appends `_<abi>` (libovt_studio_x86_64.so, §13.2.1). */
        const val QT_APP_LIBRARY = "ovt_studio"

        /** QML root for QtQuickView, which needs an Item, not the desktop ApplicationWindow (app/qml/AndroidShell.qml). */
        const val SHELL_QML = "qrc:/qt/qml/OvtStudio/AndroidShell.qml"
    }
}
