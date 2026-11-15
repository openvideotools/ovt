// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.qtproject.qt.android;

import android.os.Handler;
import android.os.Looper;
import androidx.lifecycle.Lifecycle;
import androidx.lifecycle.LifecycleOwner;

/**
 * Tells Qt 6.8 that the hosting Activity is already resumed, for a QtQuickView created after the first onResume.
 *
 * <p>OVT code, not Qt's: it lives in Qt's Java package only because QtNative's application-state API is
 * package-private. QtEmbeddedDelegate, which the first QtQuickView constructs, sets the Qt application state to Active
 * from its ActivityLifecycleCallbacks.onActivityResumed. When the view is created after that callback has fired
 * (MainActivity creates it once the window is attached, issue #547), the state stays Suspended, and every action Qt
 * queues for the UI thread (creating the root window, the display metrics) waits forever.
 *
 * <p>The state must not turn Active before Qt's platform plugin is ready: the queued root-window action calls
 * QMetaObject::invokeMethod(qApp, ...), which silently does nothing while main() has not yet constructed the
 * QGuiApplication, and the QML root then never loads. So this waits for QtNative's native-plugin-integration-ready
 * notification (sent from the platform integration's constructor, when qApp exists) and only then does what the
 * missed callback would have done. Re-check it on every Qt version change: Qt 6.9+ may rename these internals, and
 * the build then fails here.
 */
public final class OvtQtResume {
    private OvtQtResume() {}

    /**
     * Call on the UI thread right after constructing the QtQuickView. Sets the Qt state to Active as soon as Qt's
     * platform plugin is ready, if {@code owner} is resumed at that moment; otherwise Qt's own callback does it at
     * the next onResume.
     */
    public static void activityAlreadyResumed(LifecycleOwner owner) {
        final Handler ui = new Handler(Looper.getMainLooper());
        final QtNative.AppStateDetailsListener listener =
            new QtNative.AppStateDetailsListener() {
                @Override
                public void onNativePluginIntegrationReadyChanged(boolean ready) {
                    // Runs on the Qt thread while QtNative iterates its listeners: unregister later, on the UI
                    // thread.
                    if (ready)
                        ui.post(() -> {
                            QtNative.unregisterAppStateListener(this);
                            activate(owner);
                        });
                }
            };
        QtNative.registerAppStateListener(listener);
        // The plugin may have become ready before the listener was registered.
        if (QtNative.getStateDetails().nativePluginIntegrationReady)
            ui.post(() -> {
                QtNative.unregisterAppStateListener(listener);
                activate(owner);
            });
    }

    private static void activate(LifecycleOwner owner) {
        QtNative.ApplicationStateDetails details = QtNative.getStateDetails();
        if (!details.isStarted || details.state == QtNative.ApplicationState.ApplicationActive)
            return;
        if (!owner.getLifecycle().getCurrentState().isAtLeast(Lifecycle.State.RESUMED))
            return;
        QtNative.setApplicationState(QtNative.ApplicationState.ApplicationActive);
        QtNative.updateWindow();
    }
}
