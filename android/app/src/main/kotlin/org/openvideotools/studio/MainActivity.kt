// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Intent
import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
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
    private lateinit var filePicker: FilePicker
    private var overlay: DebugInfoOverlay? = null

    /** The `ovt_jni_open_fd` session of the file on screen, 0 for none. Touched on the main thread only. */
    private var session = 0L

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

        // File opening (WI-0.051): picker, VIEW/SEND intents, drag-and-drop; the debug overlay on debuggable builds.
        filePicker = FilePicker(this, ::onFileOpened)
        filePicker.attachDropTarget(host)
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) {
            val debugOverlay = DebugInfoOverlay(this, filePicker::pick)
            overlay = debugOverlay
            host.addView(debugOverlay, debugOverlay.topStartParams())
        }
        filePicker.handleIntent(intent)

        Log.i(TAG, "OVT: native ${OvtNative.version()}")
    }

    // launchMode singleTask: a second VIEW/SEND reaches the running Activity here.
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        filePicker.handleIntent(intent)
    }

    // Ctrl+O opens the picker (ChromeOS and hardware keyboards) until the Qt shell has its own File menu.
    override fun onKeyDown(
        keyCode: Int,
        event: KeyEvent,
    ): Boolean {
        if (keyCode == KeyEvent.KEYCODE_O && event.isCtrlPressed) {
            filePicker.pick()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onDestroy() {
        filePicker.shutdown()
        OvtNative.close(session)
        session = 0L
        super.onDestroy()
    }

    private fun onFileOpened(opened: FilePicker.OpenedFile) {
        OvtNative.close(session) // the previous file, if any
        session = opened.session
        overlay?.show(OvtNative.infoJson(session))
    }

    private companion object {
        const val TAG = "OVT"

        /** The Qt application library: Qt appends `_<abi>` (libovt_studio_x86_64.so, §13.2.1). */
        const val QT_APP_LIBRARY = "ovt_studio"

        /** QML root for QtQuickView, which needs an Item, not the desktop ApplicationWindow (app/qml/AndroidShell.qml). */
        const val SHELL_QML = "qrc:/qt/qml/OvtStudio/AndroidShell.qml"
    }
}
