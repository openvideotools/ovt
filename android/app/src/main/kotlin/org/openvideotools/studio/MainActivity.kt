// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.ApplicationInfo
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Display
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
    private lateinit var filePicker: FilePicker
    private var overlay: DebugInfoOverlay? = null

    /** The `ovt_jni_open_fd` session of the file on screen, 0 for none. Touched on the main thread only. */
    private var session = 0L
    private val headroomListener =
        Consumer<Display> { changed -> OvtNative.setHeadroom(hdrSurface.session, changed.hdrSdrRatio) }
    private var headroomDisplay: Display? = null

    /** True only inside a finishing onDestroy (issue #614). */
    private var keepQtAlive = false

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

        // Top child: the Qt Quick UI, created once the window's decor view is attached (see addQtView).
        window.decorView.doOnAttach { addQtView(host) }

        // File opening (WI-0.051): picker, VIEW/SEND intents, drag-and-drop; the debug overlay on debuggable builds
        // (addQtView brings it back in front of the Qt view, which is added later).
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
        // Issue #614: keep Qt running when the Activity finishes (see isChangingConfigurations).
        keepQtAlive = isFinishing
        try {
            super.onDestroy()
        } finally {
            keepQtAlive = false
        }
    }

    /**
     * Issue #614. Qt 6.8's QtEmbeddedDelegate quits Qt from Application.ActivityLifecycleCallbacks.onActivityDestroyed
     * unless isChangingConfigurations() is true. In embedded mode that quit waits on the UI thread for Qt's main() to
     * return, which nothing asks it to do, so a finishing Activity hangs (and if main() did return, Qt would call
     * exit()). Qt is kept for the life of the process instead: during a finishing onDestroy this reports a
     * configuration change, so Qt treats the finish as it treats a recreation, and the next MainActivity's QtQuickView
     * attaches to the running Qt. No ViewModel or retained Fragment state depends on this value here.
     */
    override fun isChangingConfigurations(): Boolean = keepQtAlive || super.isChangingConfigurations()

    private fun onFileOpened(opened: FilePicker.OpenedFile) {
        OvtNative.close(session) // the previous file, if any
        session = opened.session
        overlay?.show(OvtNative.infoJson(session))
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

        /** Root property of AndroidShell.qml holding the product name it displays. */
        const val SHELL_TITLE_PROPERTY = "productName"
    }
}
