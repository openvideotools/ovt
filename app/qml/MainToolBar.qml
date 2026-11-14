// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The toolbar of the main window (chapter 11 §11.2.4; WI-1.450). Factored out of Main.qml by WI-A.450 so that the
// desktop ApplicationWindow and the Android Item root (AndroidShell.qml) show the same toolbar.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

ToolBar {
    id: toolBar

    // The Jobs indicator was clicked: the shell opens or closes the Jobs popover.
    signal jobsClicked()

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
        ToolButton {
            objectName: "jobsButton"
            text: JobsModel.activeCount > 0 ? "Jobs (" + JobsModel.activeCount + ")" : "Jobs"
            Accessible.name: "Jobs"
            onClicked: toolBar.jobsClicked()
        }
    }
}
