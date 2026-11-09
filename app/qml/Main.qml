// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Main window of OVT Studio (chapter 11 §11.2.1, §11.2.3, §11.2.4; WI-1.450): menu bar, toolbar, an empty central
// area and the status bar. Every action whose feature is not built yet is disabled (Actions.qml). The title comes
// from main.cpp (OVT_PRODUCT_NAME, chapter 15 §15.7.4), so no product name is written here.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

ApplicationWindow {
    id: root

    // A file path from the command line (main.cpp): opened once the window exists (FR-GUI-027).
    property string initialFile: ""
    // The Open-as dialog, for tests.
    readonly property var openAsDialogItem: openAsDialog

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

    Component.onCompleted: {
        if (initialFile !== "")
            Project.openFile(initialFile)
    }

    // A second file while one is open: ask how to open it (§11.8).
    Connections {
        target: Project
        function onSecondFileRequested(path) {
            openAsDialog.path = path
            openAsDialog.open()
        }
    }

    // Drag-and-drop of files onto the window (FR-GUI-027); .ovtproj files load as projects.
    DropArea {
        objectName: "dropArea"
        anchors.fill: parent
        onDropped: drop => {
            for (const url of drop.urls)
                Project.openFile(url.toString())
        }
    }

    OpenAsDialog {
        id: openAsDialog

        parent: Overlay.overlay
        onNewWindowRequested: path => Project.launchNewWindow(path)
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
            ToolButton {
                objectName: "jobsButton"
                text: JobsModel.activeCount > 0 ? "Jobs (" + JobsModel.activeCount + ")" : "Jobs"
                Accessible.name: "Jobs"
                onClicked: jobsPopover.opened ? jobsPopover.close() : jobsPopover.open()
            }
        }
    }

    // Window-level shortcuts of the Keymap (§11.4.3, WI-1.466) and the dialogs they open (Go to, Preferences, Keyboard
    // map). Ctrl+J (action "view.jobs") opens the Jobs popover (§11.3.18).
    ShortcutLayer {
        actions: actions
        blocked: dialogs.anyOpen
    }

    Dialogs {
        id: dialogs

        actions: actions
        overlayParent: Overlay.overlay
    }

    Connections {
        target: actions

        function onActivated(id) {
            if (id === "view.jobs")
                jobsPopover.opened ? jobsPopover.close() : jobsPopover.open()
        }
    }

    JobsPopover {
        id: jobsPopover

        parent: Overlay.overlay
        x: parent.width - width - OvtStyle.spacing
        y: root.header.height + OvtStyle.spacing
    }

    // GUI-JOB-0001 (§11.7): a job that did not stop in time is detached and reported.
    Connections {
        target: JobsModel
        function onJobDetached(name) {
            DiagnosticsModel.addGui(1, "GUI-JOB-0001", "job " + name + " did not stop within 2 s; detached")
        }
    }

    // The dock areas and panels (§11.2.2, WI-1.451). Focus panel N and tab cycling come in through actions.
    DockWorkspace {
        id: dockWorkspace

        anchors.fill: parent
        actions: actions
        panelComponents: ({"diagnostics": diagnosticsComponent})
    }

    // The Diagnostics / Log panel (§11.3.16) is a dock panel in the Bottom area; the status bar badge toggles it.
    Component {
        id: diagnosticsComponent

        DiagnosticsPanel {}
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
}
