// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The layouts by width of chapter 11 §11.5.2 (WI-A.451): hosts the dock workspace and picks its layout class from
// `Platform.widthDp` (smallest-width breakpoints): three-pane at >= 1200 dp (the desktop skeleton unchanged),
// two-pane at 840-1199 dp (picture 60 % + tabbed area, compact timeline, the left area behind the hamburger as a
// drawer), vertical at 600-839 dp (picture on top, 96 dp timeline strip, tabbed area below) and a single pane with
// bottom navigation below 600 dp (Picture, Timeline, Info, More; unsupported but it must not crash).
//
// A switch changes only how the one DockWorkspace arranges its areas. The panel items, Selection, Project and the
// jobs all outlive it, which is the "no state lost on switch" rule of §11.5.2 and §11.5.7 (step 4 of the WI).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

ColumnLayout {
    id: root

    // The shell's Actions and the panel components for the workspace (AppShell.qml).
    property var actions: null
    property var panelComponents: ({})
    // Compact layouts apply on Android only; the desktop keeps the three-pane skeleton at every width (§11.5.2 is
    // an Android rule). Tests turn it on to run the breakpoints against a mocked Platform.widthDp.
    property bool active: Platform.isAndroid
    // Re-reads Platform from the window's size when it changes (Android; §11.5.3: ≤ 1 frame, no Activity recreation).
    property bool trackWindow: Platform.isAndroid
    property real widthDp: Platform.widthDp
    readonly property string layoutClass: active ? Platform.layoutClassForWidth(widthDp) : "three"
    readonly property alias workspace: workspace

    spacing: 0

    // Qt's logical pixels are dp on Android (density scaling is always on, §11.5.4); the platform value wins.
    onWidthChanged: if (trackWindow) Platform.refresh(width)

    // Logged for the emulator test (test_android_breakpoints.py): the class, the width it came from and the selected frame.
    function logLayout() {
        console.log("OVT: layout " + layoutClass + " widthDp=" + Math.round(widthDp) + " frame=" + Selection.frameDecodeIndex)
    }
    onLayoutClassChanged: if (active) logLayout()
    Component.onCompleted: if (active) logLayout()

    // The drawer button of the two-pane and vertical layouts.
    ToolBar {
        objectName: "compactBar"
        visible: root.layoutClass === "two" || root.layoutClass === "vertical"
        Layout.fillWidth: true
        implicitHeight: 48

        ToolButton {
            objectName: "drawerButton"
            anchors.verticalCenter: parent.verticalCenter
            text: "☰"
            checkable: true
            checked: workspace.drawerOpen
            onClicked: workspace.drawerOpen = !workspace.drawerOpen
            Accessible.name: qsTr("Panels drawer")
            implicitWidth: 48
            implicitHeight: 48
        }
    }

    DockWorkspace {
        id: workspace

        Layout.fillWidth: true
        Layout.fillHeight: true
        actions: root.actions
        panelComponents: root.panelComponents
        layoutClass: root.layoutClass
        // The drawer does not outlive a switch to the three-pane layout, where the left area shows on its own.
        onLayoutClassChanged: if (layoutClass === "three") drawerOpen = false
    }

    // Single pane: bottom navigation, one area full-screen at a time.
    TabBar {
        id: nav

        objectName: "bottomNav"
        visible: root.layoutClass === "single"
        Layout.fillWidth: true
        currentIndex: workspace.singleIndex
        onCurrentIndexChanged: workspace.singleIndex = currentIndex

        Repeater {
            model: [qsTr("Picture"), qsTr("Timeline"), qsTr("Info"), qsTr("More")]
            delegate: TabButton {
                required property string modelData

                text: modelData
                implicitHeight: 48
            }
        }
    }
}
