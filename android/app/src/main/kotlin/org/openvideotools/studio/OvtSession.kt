// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.openvideotools.studio

import android.os.Handler
import android.os.Looper
import androidx.annotation.MainThread
import androidx.annotation.VisibleForTesting

/**
 * Process-lifetime owner of the native core handles (chapter 11 §11.5.7; WI-A.450). The `ovt_jni_open_fd` session
 * (its `ovt_File` and `ovt_Container`, and the caches behind them) belongs to the process, not to [MainActivity]:
 * when the system destroys and recreates the Activity (a configuration change the manifest does not absorb, or
 * "Don't keep activities"), the new Activity re-binds to the open file here instead of opening and indexing it
 * again. The handles are closed only by [close], which the Activity calls when it is finishing, and by [adopt] when a
 * new file replaces the open one.
 *
 * SPEC-ASSUMPTION(#443): the package is `org.openvideotools.studio` (backbone D5, chapter 13 §13.4.4), not the
 * `org/ovt/` path that chapter 11 names for this file.
 *
 * All state is touched on the main thread only.
 */
object OvtSession {
    /** A file opened in the native core: the `ovt_jni_open_fd` handle and the name the provider gave. */
    class OpenFile(
        val handle: Long,
        val displayName: String,
    )

    /** Told when the open file changes; the bound Activity updates its views from here. */
    fun interface Listener {
        @MainThread
        fun onFileChanged(file: OpenFile?)
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private var listener: Listener? = null

    /** The open file, or null. */
    @get:MainThread
    var current: OpenFile? = null
        private set

    /**
     * How many files this process has opened through [adopt]. A re-bind after Activity recreation does not change
     * it; SessionTest uses it to show that nothing was opened (and so indexed) again. SPEC-ASSUMPTION(#444): it
     * stands in for the `source_hash` comparison until an `ovt_Stream`/index joins the session.
     */
    @get:MainThread
    var openCount: Int = 0
        private set

    /**
     * Binds the Activity's [listener] (called from `onCreate`, also after a recreation) and tells it the file
     * already open, if any. Replaces the previous listener, which belongs to a destroyed Activity.
     */
    @MainThread
    fun bind(listener: Listener) {
        checkMainThread()
        this.listener = listener
        listener.onFileChanged(current)
    }

    /** Unbinds [listener] if it is still the bound one (called from `onDestroy`); the handles stay open. */
    @MainThread
    fun unbind(listener: Listener) {
        checkMainThread()
        if (this.listener === listener) this.listener = null
    }

    /**
     * Takes ownership of a newly opened session [handle] (from `OvtNative.openFd`, never 0), closing the file open
     * before it. Safe to call from any thread: the hand-over runs on the main thread, so a file that finishes opening
     * while the Activity is being recreated reaches the new Activity.
     */
    fun adopt(
        handle: Long,
        displayName: String,
    ) {
        require(handle != 0L) { "adopt: no session" }
        runOnMain {
            val previous = current
            current = OpenFile(handle, displayName)
            openCount += 1
            if (previous != null) OvtNative.close(previous.handle)
            listener?.onFileChanged(current)
        }
    }

    /** Closes the open file, if any (the Activity is finishing, or File ▸ Close). */
    @MainThread
    fun close() {
        checkMainThread()
        val previous = current ?: return
        current = null
        OvtNative.close(previous.handle)
        listener?.onFileChanged(null)
    }

    /** Test hook: the listener bound now. */
    @VisibleForTesting
    internal val boundListener: Listener?
        get() = listener

    private fun runOnMain(block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) block() else mainHandler.post(block)
    }

    private fun checkMainThread() {
        check(Looper.myLooper() == Looper.getMainLooper()) { "OvtSession is main-thread only" }
    }
}
