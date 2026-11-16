// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The status bar of the main window (chapter 11 §11.2.4; WI-1.450, WI-1.453). Factored out of Main.qml by WI-A.450 so
// that the desktop ApplicationWindow and the Android Item root (AndroidShell.qml) show the same status bar.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

ToolBar {
    id: statusBar

    objectName: "statusBar"
    Accessible.name: "Status bar"
    position: ToolBar.Footer

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: OvtStyle.spacing
        anchors.rightMargin: OvtStyle.spacing
        spacing: OvtStyle.spacing * 2

        Label {
            text: Project.fileCount > 0 ? Project.files.map(f => f.name).join("  |  ") : "No file open"
            color: OvtStyle.textMuted
            objectName: "statusFile"
        }
        Item { Layout.fillWidth: true }
        Label { text: "100 %"; color: OvtStyle.textMuted; objectName: "statusZoom" }
        Label {
            objectName: "statusJobText"
            visible: JobsModel.foregroundActive
            text: JobsModel.foregroundText
            color: OvtStyle.textMuted
            elide: Text.ElideRight
            Layout.maximumWidth: 420
        }
        // Job progress bar: hidden when idle (§11.2.4); indeterminate while the total is unknown.
        ProgressBar {
            objectName: "statusJobs"
            visible: JobsModel.foregroundActive
            from: 0
            to: 1
            value: Math.max(0, JobsModel.foregroundProgress)
            indeterminate: JobsModel.foregroundActive && JobsModel.foregroundProgress < 0
            Accessible.name: "Job progress"
        }
        // The Diagnostics badge: red when Errors > 0 (§11.3.16). Click opens or closes the Diagnostics panel.
        ToolButton {
            objectName: "statusDiagnostics"
            text: DiagnosticsModel.errorCount > 0 ? DiagnosticsModel.errorCount + " errors" : "Diagnostics"
            onClicked: DockModel.togglePanel("diagnostics")
            Accessible.name: "Diagnostics"
            palette.buttonText: DiagnosticsModel.errorCount > 0 ? "#ff6b6b" : OvtStyle.text
        }
    }
}
