// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.ApplicationInfo
import android.content.res.Configuration
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
 *
 * The open file is not owned here: [OvtSession] keeps the core handles for the life of the process, and every
 * instance of this Activity binds to it in [onCreate] (WI-A.450, chapter 11 §11.5.7), so a recreated Activity shows
 * the same file without opening or indexing it again.
 */
class MainActivity : AppCompatActivity() {
    private lateinit var hdrSurface: HdrSurfaceView
    private lateinit var qtView: QtQuickView

    /** Opening documents (WI-0.051) and the recent list (WI-A.452); internal for SafFlowsTest. */
    internal lateinit var filePicker: FilePicker
        private set
    internal lateinit var recentFiles: RecentFiles
        private set

    /** The `GUI-AND-0001` prompt and its copy action (WI-A.452); internal for SafFlowsTest. */
    internal lateinit var copyPrompt: CopyPrompt
        private set
    private lateinit var exporter: ExportDocument
    private var overlay: DebugInfoOverlay? = null

    /** This instance's binding to [OvtSession]: shows the open file in the debug overlay. */
    private val sessionListener =
        OvtSession.Listener { file -> overlay?.show(file?.let { OvtNative.infoJson(it.handle) }) }
    private val recentListener = RecentFiles.Listener { entries -> overlay?.showRecent(entries) }
    private val headroomListener =
        Consumer<Display> { changed -> OvtNative.setHeadroom(hdrSurface.session, changed.hdrSdrRatio) }
    private var headroomDisplay: Display? = null

    /**
     * True once Qt's start-up actions have run (OvtQtResume.whenStartupActionsRan); internal for the tests. Until
     * then [finish] waits: finishing earlier detaches the window under Qt's pending display-metrics action, which then
     * crashes on the null insets (WI-A.452, found by SafFlowsTest on the Mini).
     */
    internal var qtStarted = false
        private set
    private var finishWhenQtStarted = false

    /** True only inside a finishing onDestroy (issue #614). */
    private var keepQtAlive = false

    /** True once the QML root is READY: QtQuickView.setProperty needs the root (WI-A.451). */
    private var shellReady = false

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

        // Bottom child: the HDR picture surface with the PQ test pattern of WI-0.052, below the top quarter. The opaque
        // WI-A.450 shell covers it until WI-A.400 gives it the Picture panel's geometry.
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
        // (addQtView brings it back in front of the Qt view, which is added later). A newly opened file goes to
        // OvtSession, which closes the one before it. WI-A.452 (§11.5.6): the recent list, GUI-AND-0001 with the
        // copy action for a provider that cannot seek, and export through ACTION_CREATE_DOCUMENT.
        recentFiles = RecentFiles(this)
        copyPrompt = CopyPrompt(this, CopyToAppStorage(this)) { file, name -> filePicker.openCopy(file, name) }
        filePicker =
            FilePicker(
                this,
                recentFiles,
                onOpened = { opened -> OvtSession.adopt(opened.session, opened.displayName) },
                onNotSeekable = copyPrompt::show,
            )
        filePicker.attachDropTarget(host)
        exporter = ExportDocument(this)
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) {
            val debugOverlay = DebugInfoOverlay(this, filePicker::pick, ::exportInfo, filePicker::reopen)
            overlay = debugOverlay
            host.addView(debugOverlay, debugOverlay.topStartParams())
        }
        recentFiles.addListener(recentListener)

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
        OvtQtResume.whenStartupActionsRan {
            qtStarted = true
            if (finishWhenQtStarted) {
                finishWhenQtStarted = false
                Log.i(TAG, "OVT: Qt started, finishing now")
                super.finish()
            }
        }
        overlay?.bringToFront() // the WI-0.051 debug overlay stays above the Qt view
    }

    /** Called on the UI thread whenever the QML root's status changes (QtQmlStatusChangeListener). */
    private fun onShellStatus(status: QtQmlStatus) {
        when (status) {
            QtQmlStatus.READY -> {
                exposeShellTitle()
                shellReady = true
                pushWindowWidth(resources.configuration) // a resize while the QML loaded
            }
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

    /**
     * WI-A.451 (§11.5.2): a resize (split-screen, freeform, desktop windowing) changes the Configuration without
     * recreating the Activity (manifest configChanges). The QML root gets the window width in dp from here, when
     * Android reports it, rather than by polling or from a QML item width (device pixels on Android).
     */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        pushWindowWidth(newConfig)
    }

    private fun pushWindowWidth(config: Configuration) {
        Log.i(TAG, "OVT: configuration screenWidthDp=${config.screenWidthDp} shellReady=$shellReady")
        if (shellReady && config.screenWidthDp != Configuration.SCREEN_WIDTH_DP_UNDEFINED) {
            qtView.setProperty(SHELL_WIDTH_PROPERTY, config.screenWidthDp)
        }
    }

    // launchMode singleTask: a second VIEW/SEND reaches the running Activity here, without recreating it.
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        filePicker.handleIntent(intent)
    }

    /** Exports the open file's info JSON (the debug overlay's "Export info…"; WI-A.452 export flow). */
    private fun exportInfo() {
        val file = OvtSession.current ?: return
        val json = OvtNative.infoJson(file.handle) ?: return
        exporter.export(
            ExportDocument.Request("${file.displayName}.info.json", "application/json") { out ->
                out.write(json.toByteArray(Charsets.UTF_8))
            },
        )
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

    /**
     * Defers the finish of an Activity whose Qt view is still starting until Qt's start-up actions have run (see
     * [qtStarted]); a user backing out right after launch would otherwise crash the app inside Qt 6.8.
     */
    override fun finish() {
        if (::qtView.isInitialized && !qtStarted) {
            Log.i(TAG, "OVT: finish deferred until Qt has started")
            finishWhenQtStarted = true
            return
        }
        super.finish()
    }

    // The handles outlive a recreation; they are closed only when the Activity really finishes (§11.5.7: jobs are
    // cancelled on onDestroy with isFinishing).
    override fun onDestroy() {
        filePicker.shutdown()
        exporter.shutdown()
        copyPrompt.cancel()
        recentFiles.removeListener(recentListener)
        OvtSession.unbind(sessionListener)
        if (isFinishing) OvtSession.close()
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

    private companion object {
        const val TAG = "OVT"

        /** Top margin of the HDR surface as a share of the window height, until WI-A.400 follows the Picture panel. */
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

        /** Root property of AndroidShell.qml taking Configuration.screenWidthDp (WI-A.451). */
        const val SHELL_WIDTH_PROPERTY = "configurationWidthDp"
    }
}
