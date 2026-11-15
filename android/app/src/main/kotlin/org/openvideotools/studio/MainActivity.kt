// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.os.Bundle
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.doOnAttach
import org.qtproject.qt.android.QtQmlStatus
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

        // Top child: the Qt Quick UI, created once the window's decor view is attached (see addQtView).
        window.decorView.doOnAttach { addQtView(host) }

        Log.i(TAG, "OVT: native ${OvtNative.version()}")
    }

    /**
     * Adds the Qt Quick UI. Constructing QtQuickView loads the Qt libraries and libovt_studio_<abi>.so, runs its
     * main() (which only starts the event loop on Android) and then loads the QML root.
     *
     * Not done in onCreate: as soon as Qt's platform plugin is ready, Qt 6.8's QtEmbeddedDelegate posts
     * QtDisplayManager.setApplicationDisplayMetrics() to the UI thread, which dereferences the decor view's
     * getRootWindowInsets() without a null check. Before the first traversal attaches the window those insets are
     * null, so a QtQuickView built in onCreate crashed with a NullPointerException in about half the emulator runs.
     * After attachment the insets are never null.
     */
    private fun addQtView(host: FrameLayout) {
        qtView = QtQuickView(this, SHELL_QML, QT_APP_LIBRARY)
        qtView.setStatusChangeListener { status -> onShellStatus(status) }
        host.addView(
            qtView,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )
    }

    /** Called on the UI thread whenever the QML root's status changes (QtQmlStatusChangeListener). */
    private fun onShellStatus(status: QtQmlStatus) {
        when (status) {
            QtQmlStatus.READY -> exposeShellTitle()
            QtQmlStatus.ERROR -> Log.e(TAG, "OVT-FATAL: the QML root $SHELL_QML failed to load")
            else -> Unit
        }
    }

    /**
     * Gives the Qt view an accessibility node that carries the product name the loaded QML root shows.
     *
     * Qt 6.8 exposes the Qt Quick accessibility tree only through its own QtActivity: QtEmbeddedDelegate, which
     * backs QtQuickView, registers no QtAccessibilityInterface (nor in Qt 6.9 or 6.10), so TalkBack and UI Automator
     * see the embedded view as one opaque node (issue #547). Until Qt supports it, the view itself carries the root's
     * `productName` property as its text and content description. The value is read from the QML root once it is
     * READY, so the node appears only after the QML has loaded, and it holds what the QML shows, not a copy of the
     * Android string resource.
     */
    private fun exposeShellTitle() {
        val title = qtView.getProperty<String?>(SHELL_TITLE_PROPERTY)
        if (title.isNullOrEmpty()) {
            Log.w(TAG, "OVT: the QML root has no '$SHELL_TITLE_PROPERTY' property; no accessibility title")
            return
        }
        qtView.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
        qtView.contentDescription = title
        qtView.accessibilityDelegate =
            object : View.AccessibilityDelegate() {
                override fun onInitializeAccessibilityNodeInfo(
                    host: View,
                    info: AccessibilityNodeInfo,
                ) {
                    super.onInitializeAccessibilityNodeInfo(host, info)
                    info.text = title
                }
            }
        Log.i(TAG, "OVT: QML root ready, title '$title'")
    }

    private companion object {
        const val TAG = "OVT"

        /** The Qt application library: Qt appends `_<abi>` (libovt_studio_x86_64.so, §13.2.1). */
        const val QT_APP_LIBRARY = "ovt_studio"

        /** QML root for QtQuickView, which needs an Item, not the desktop ApplicationWindow (app/qml/AndroidShell.qml). */
        const val SHELL_QML = "qrc:/qt/qml/OvtStudio/AndroidShell.qml"

        /** Root property of AndroidShell.qml holding the product name it displays. */
        const val SHELL_TITLE_PROPERTY = "productName"
    }
}
