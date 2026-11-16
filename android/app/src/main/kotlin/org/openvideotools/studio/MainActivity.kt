// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Intent
import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.doOnAttach
import androidx.lifecycle.Lifecycle
import org.qtproject.qt.android.OvtQtResume
import org.qtproject.qt.android.QtQmlStatus
import org.qtproject.qt.android.QtQuickView

/**
 * The single Activity of OVT Studio (chapter 13 §13.4.4; WI-0.050). A plain Android Activity that embeds Qt Quick
 * through [QtQuickView] (ADR-004), not Qt's own QtActivity, so that it can own the HDR SurfaceView sibling
 * (ADR-014). WI-0.050 sets up the layout of item 1 and the JNI library; the later items arrive with WI-0.051
 * (files), WI-0.052 (HDR output), WI-0.053 (plugins) and WI-0.054 (ChromeOS configuration changes).
 *
 * The open file is not owned here: [OvtSession] keeps the core handles for the life of the process, and every
 * instance of this Activity binds to it in [onCreate] (WI-A.450, chapter 11 §11.5.7), so a recreated Activity shows
 * the same file without opening or indexing it again.
 */
class MainActivity : AppCompatActivity() {
    private lateinit var hdrSurface: HdrSurfaceView
    private lateinit var qtView: QtQuickView
    private lateinit var filePicker: FilePicker
    private var overlay: DebugInfoOverlay? = null

    /** This instance's binding to [OvtSession]: shows the open file in the debug overlay. */
    private val sessionListener =
        OvtSession.Listener { file -> overlay?.show(file?.let { OvtNative.infoJson(it.handle) }) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val host = findViewById<FrameLayout>(R.id.ovt_host)

        // Bottom child: the HDR picture surface, positioned by the Picture panel's geometry from WI-0.052 on.
        hdrSurface = HdrSurfaceView(this)
        host.addView(hdrSurface, FrameLayout.LayoutParams(0, 0))

        // Top child: the Qt Quick UI, created once the window's decor view is attached (see addQtView).
        window.decorView.doOnAttach { addQtView(host) }

        // File opening (WI-0.051): picker, VIEW/SEND intents, drag-and-drop; the debug overlay on debuggable builds
        // (addQtView brings it back in front of the Qt view, which is added later). A newly opened file goes to
        // OvtSession, which closes the one before it.
        filePicker = FilePicker(this) { opened -> OvtSession.adopt(opened.session, opened.displayName) }
        filePicker.attachDropTarget(host)
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) {
            val debugOverlay = DebugInfoOverlay(this, filePicker::pick)
            overlay = debugOverlay
            host.addView(debugOverlay, debugOverlay.topStartParams())
        }

        // Re-bind to the handles of the process (WI-A.450). The launch intent is handled on a fresh start only: a
        // recreated Activity gets the same intent again, and opening it would replace (and re-index) the file that
        // OvtSession still holds. After process death OvtSession is empty, so the intent's file is opened again.
        OvtSession.bind(sessionListener)
        if (savedInstanceState == null || OvtSession.current == null) {
            filePicker.handleIntent(intent)
        }

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
     *
     * The window attaches after the first onResume, so Qt's own resume callback has already been missed: OvtQtResume
     * sets the Qt application state to Active, which runs the actions Qt queued (root window, display metrics).
     */
    private fun addQtView(host: FrameLayout) {
        qtView = QtQuickView(this, SHELL_QML, QT_APP_LIBRARY)
        qtView.setStatusChangeListener { status -> onShellStatus(status) }
        host.addView(
            qtView,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )
        // Otherwise Qt's ActivityLifecycleCallbacks set the state at the next onResume.
        if (lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) OvtQtResume.activityAlreadyResumed()
        overlay?.bringToFront() // the WI-0.051 debug overlay stays above the Qt view
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

    // launchMode singleTask: a second VIEW/SEND reaches the running Activity here, without recreating it.
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

    // The handles outlive a recreation; they are closed only when the Activity really finishes (§11.5.7: jobs are
    // cancelled on onDestroy with isFinishing).
    override fun onDestroy() {
        filePicker.shutdown()
        OvtSession.unbind(sessionListener)
        if (isFinishing) OvtSession.close()
        super.onDestroy()
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
