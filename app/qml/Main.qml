// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Main window of OVT Studio (chapter 11 §11.2.1, §11.2.3, §11.2.4; WI-1.450): menu bar, toolbar, the shell and the
// status bar. Every action whose feature is not built yet is disabled (Actions.qml). The title comes from main.cpp
// (OVT_PRODUCT_NAME, chapter 15 §15.7.4), so no product name is written here. The window content lives in
// AppShell.qml, MainToolBar.qml and StatusBar.qml, which Android's Item root (AndroidShell.qml) shares (WI-A.450).

import QtQuick
import QtQuick.Controls
import OvtStudio

ApplicationWindow {
    id: root

    // A file path from the command line (main.cpp): opened once the window exists (FR-GUI-027).
    property string initialFile: ""
    // The Open-as dialog, for tests.
    readonly property var openAsDialogItem: shell.openAsDialogItem

    width: 1280
    height: 800
    minimumWidth: 1024
    minimumHeight: 640
    visible: true
    color: OvtStyle.window

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
        placeholderText: OvtStyle.textMuted
        mid: OvtStyle.border
        dark: OvtStyle.border
        light: OvtStyle.panel
        toolTipBase: OvtStyle.panel
        toolTipText: OvtStyle.text
    }
    font.pixelSize: OvtStyle.fontSize

    Component.onCompleted: {
        if (initialFile !== "")
            Project.openFile(initialFile)
    }

    menuBar: OvtMenuBar {
        objectName: "menuBar"
        actions: shell.actions
    }

    header: MainToolBar {
        id: toolBar

        onJobsClicked: shell.toggleJobs()
    }

    AppShell {
        id: shell

        anchors.fill: parent
    }

    footer: StatusBar {
        id: statusBar
    }
}
