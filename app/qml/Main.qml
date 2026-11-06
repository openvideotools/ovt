// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Main window of OVT Studio (chapter 11 §11.2.1, §11.2.3, §11.2.4; WI-1.450): menu bar, toolbar, an empty central
// area and the status bar. Every action whose feature is not built yet is disabled (Actions.qml). The title comes
// from main.cpp (OVT_PRODUCT_NAME, chapter 15 §15.7.4), so no product name is written here.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

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

    Actions {
        id: actions
    }

    menuBar: OvtMenuBar {
        objectName: "menuBar"
        actions: actions
    }

    header: ToolBar {
        id: toolBar

        objectName: "toolBar"
        Accessible.name: "Toolbar"

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: OvtStyle.spacing
            anchors.rightMargin: OvtStyle.spacing
            spacing: 2

            // Left to right as in §11.2.4; each control is disabled until its work item lands.
            ToolButton { text: "Open"; enabled: false }
            ToolButton { text: "Open Reference"; enabled: false }
            ToolSeparator {}
            ToolButton { text: "Play/Pause"; enabled: false }
            ToolButton { text: "Step ◁"; enabled: false }
            ToolButton { text: "Step ▷"; enabled: false }
            SpinBox { enabled: false; editable: true; Layout.preferredWidth: 84; Accessible.name: "Frame" }
            ToolButton { text: "Dec/Disp"; enabled: false }
            ToolSeparator {}
            ToolButton { text: "Zoom −"; enabled: false }
            ToolButton { text: "Fit"; enabled: false }
            ToolButton { text: "Zoom +"; enabled: false }
            ComboBox { model: ["YUV", "Y", "U", "V", "Gray"]; enabled: false; Layout.preferredWidth: 80; Accessible.name: "Plane" }
            ToolSeparator {}
            ComboBox { model: ["Overlay mode"]; enabled: false; Layout.preferredWidth: 120; Accessible.name: "Overlay mode" }
            ToolSeparator {}
            ComboBox { model: ["Single"]; enabled: false; Layout.preferredWidth: 84; Accessible.name: "Compare mode" }
            ToolSeparator {}
            ToolButton { text: "HDR output"; checkable: true; enabled: false }
            Item { Layout.fillWidth: true }
            ToolButton { text: "Jobs"; enabled: false; Accessible.name: "Jobs" }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: OvtStyle.window

        Label {
            anchors.centerIn: parent
            text: "No file open"
            color: OvtStyle.textMuted
            Accessible.name: "No file open"
        }
    }

    footer: ToolBar {
        id: statusBar

        objectName: "statusBar"
        Accessible.name: "Status bar"
        position: ToolBar.Footer

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: OvtStyle.spacing
            anchors.rightMargin: OvtStyle.spacing
            spacing: OvtStyle.spacing * 2

            Label { text: "No file open"; color: OvtStyle.textMuted; objectName: "statusFile" }
            Item { Layout.fillWidth: true }
            Label { text: "100 %"; color: OvtStyle.textMuted; objectName: "statusZoom" }
            // Job progress bar: hidden when idle (§11.2.4).
            ProgressBar { visible: false; value: 0; objectName: "statusJobs" }
        }
    }
}
