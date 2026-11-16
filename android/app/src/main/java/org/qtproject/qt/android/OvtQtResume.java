// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.qtproject.qt.android;

/**
 * Tells Qt 6.8 that the hosting Activity is already resumed, for a QtQuickView created after the first onResume.
 *
 * <p>OVT code, not Qt's: it lives in Qt's Java package only because QtNative's application-state API is
 * package-private.
 * QtEmbeddedDelegate, which the first QtQuickView constructs, sets the Qt application state to Active from its
 * ActivityLifecycleCallbacks.onActivityResumed. When the view is created after that callback has fired (MainActivity
 * creates it once the window is attached, issue #547), the state stays Suspended, and every action Qt queues for the
 * UI thread (creating the root window, the display metrics) waits forever. {@link #activityAlreadyResumed()} does what
 * that callback would have done. Re-check it on every Qt version change: Qt 6.9+ may rename these internals, and the
 * build then fails here.
 */
public final class OvtQtResume {
    private OvtQtResume() {}

    /** Call on the UI thread, after constructing the QtQuickView, only while the Activity is resumed. */
    public static void activityAlreadyResumed() {
        QtNative.ApplicationStateDetails details = QtNative.getStateDetails();
        if (!details.isStarted || details.state == QtNative.ApplicationState.ApplicationActive)
            return;
        QtNative.setApplicationState(QtNative.ApplicationState.ApplicationActive);
        QtNative.updateWindow();
    }
}
