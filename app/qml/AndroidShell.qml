// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The root item that MainActivity's QtQuickView loads on Android (chapter 13 §13.4.3; WI-0.050, WI-A.450).
// QtQuickView shows an Item inside the Activity and cannot take a Window, so the desktop Main.qml (an
// ApplicationWindow) is not loaded here. SPEC-ASSUMPTION(#356): WI-A.450 brings the desktop shell into the Activity
// by hosting the same pieces Main.qml hosts (OvtMenuBar, MainToolBar, AppShell, StatusBar) in a Page, an Item that
// carries the window palette and font as ApplicationWindow does on the desktop. The Welcome page (WI-1.464) is
// therefore what a fresh launch shows (WI-A.450 acceptance test 1). The breakpoint layouts of §11.5.2 are WI-A.451.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Page {
    id: root

    // The shell, for tests.
    readonly property alias shellItem: shell

    // Main.qml's window palette, less placeholderText, which a Control's palette does not offer on Qt 6.4 (the
    // oldest Qt the desktop tests build with); the controls' own default applies there.
    palette {
        window: OvtStyle.window
        windowText: OvtStyle.text
        base: OvtStyle.panel
        alternateBase: OvtStyle.window
        text: OvtStyle.text
        button: OvtStyle.panel
        buttonText: OvtStyle.text
        highlight: OvtStyle.highlight
        highlightedText: OvtStyle.highlightedText
        mid: OvtStyle.border
        dark: OvtStyle.border
        light: OvtStyle.panel
        toolTipBase: OvtStyle.panel
        toolTipText: OvtStyle.text
    }
    font.pixelSize: OvtStyle.fontSize

    background: Rectangle {
        color: OvtStyle.window
    }

    header: ColumnLayout {
        spacing: 0

        OvtMenuBar {
            objectName: "menuBar"
            Layout.fillWidth: true
            actions: shell.actions
        }
        MainToolBar {
            objectName: "toolBar"
            Layout.fillWidth: true
            onJobsClicked: shell.toggleJobs()
        }
    }

    AppShell {
        id: shell

        anchors.fill: parent
    }

    footer: StatusBar {}
}
