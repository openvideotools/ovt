// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPEC-ASSUMPTION(#356): root item that MainActivity's QtQuickView loads on Android (chapter 13 §13.4.3; WI-0.050).
// QtQuickView shows an Item inside the Activity and cannot take a Window, so the desktop Main.qml (an
// ApplicationWindow) is not loaded here; WI-A.450 brings the desktop shell into the Activity. Until then this is the
// empty window of WI-0.019: the product name, which main.cpp sets as the application display name (OVT_PRODUCT_NAME,
// chapter 15 §15.7.4), on the window colour. The text is exposed to accessibility so that UI Automator finds it
// (WI-0.050 acceptance test 2).

import QtQuick
import OvtStudio

Rectangle {
    id: root

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
