// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPEC-ASSUMPTION(#356): root item that MainActivity's QtQuickView loads on Android (chapter 13 §13.4.3; WI-0.050).
// QtQuickView shows an Item inside the Activity and cannot take a Window, so the desktop Main.qml (an
// ApplicationWindow) is not loaded here; WI-A.450 brings the desktop shell into the Activity. Until then this is the
// empty window of WI-0.019: the product name, which main.cpp sets as the application display name (OVT_PRODUCT_NAME,
// chapter 15 §15.7.4), on the window colour. The text is exposed to accessibility so that UI Automator finds it
// (WI-0.050 acceptance test 2).
//
// WI-0.052: only the header band is painted; the rest of the window is transparent, so the HDR test pattern of
// HdrSurfaceView (below QtQuickView, chapter 13 §13.4.4 item 1) shows through. MainActivity.SHELL_HEADER_FRACTION
// places the surface under the band; WI-A.400 replaces both with the Picture panel's geometry.

import QtQuick
import OvtStudio

Item {
    id: root

    // Keep in step with MainActivity.SHELL_HEADER_FRACTION.
    readonly property real headerFraction: 0.25

    Component.onCompleted: {
        if (root.Window.window)
            root.Window.window.color = "transparent";
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: parent.height * root.headerFraction
        color: OvtStyle.window

        Text {
            anchors.centerIn: parent
            text: Qt.application.displayName
            color: OvtStyle.text
            font.pixelSize: OvtStyle.fontSize * 2
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }
}
