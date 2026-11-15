// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.qtproject.qt.android;

import android.os.Handler;
import android.os.Looper;

/**
 * Qt 6.8 start-up hand-offs for a QtQuickView created after the Activity's first onResume (MainActivity creates it
 * once the window is attached, issue #547).
 *
 * <p>OVT code, not Qt's: it lives in Qt's Java package only because QtNative's application-state API is
 * package-private. Re-check it on every Qt version change: Qt 6.9+ may rename these internals, and the build then
 * fails here.
 */
public final class OvtQtResume {
    private OvtQtResume() {}

    /**
     * Sets the Qt application state to Active, as QtEmbeddedDelegate's onActivityResumed callback would have done had
     * the view existed then. Call on the UI thread right after constructing the QtQuickView, while the Activity is
     * resumed. Without it the state stays Suspended and QtNative parks every action for the UI thread, including the
     * C++ runnables Qt's start-up waits for, so Qt never finishes starting.
     */
    public static void activityAlreadyResumed() {
        QtNative.ApplicationStateDetails details = QtNative.getStateDetails();
        if (!details.isStarted || details.state == QtNative.ApplicationState.ApplicationActive)
            return;
        QtNative.setApplicationState(QtNative.ApplicationState.ApplicationActive);
        QtNative.updateWindow();
    }

    /**
     * Runs {@code action} on the UI thread once Qt's platform plugin is ready, which QtNative reports from the
     * platform integration's constructor, after main() has created the QGuiApplication; at once (posted) if it already
     * is. Attaching a QtQuickView before that loses its root window: QtView.onAttachedToWindow queues createRootWindow,
     * whose native side calls QMetaObject::invokeMethod(qApp, ...), a silent no-op while qApp is null.
     */
    public static void runWhenPluginReady(Runnable action) {
        final Handler ui = new Handler(Looper.getMainLooper());
        final boolean[] done = {false};
        final QtNative.AppStateDetailsListener listener =
            new QtNative.AppStateDetailsListener() {
                @Override
                public void onNativePluginIntegrationReadyChanged(boolean ready) {
                    // Runs on the Qt thread while QtNative iterates its listeners: act and unregister on the UI
                    // thread.
                    if (ready)
                        ui.post(() -> runOnce(this, done, action));
                }
            };
        QtNative.registerAppStateListener(listener);
        // The plugin may have become ready before the listener was registered.
        if (QtNative.getStateDetails().nativePluginIntegrationReady)
            ui.post(() -> runOnce(listener, done, action));
    }

    // UI thread only.
    private static void runOnce(QtNative.AppStateDetailsListener listener, boolean[] done, Runnable action) {
        QtNative.unregisterAppStateListener(listener);
        if (done[0])
            return;
        done[0] = true;
        action.run();
    }
}
