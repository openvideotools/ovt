// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The content of the main window below the toolbar and above the status bar (chapter 11 §11.2; WI-1.450), as an
// Item. WI-A.450 factored it out of Main.qml (issue #356): Android's QtQuickView only accepts an Item root, so the
// desktop ApplicationWindow (Main.qml) and the Android root (AndroidShell.qml) both host this one shell, together with
// OvtMenuBar, MainToolBar and StatusBar. It owns the Actions, the dialogs, the Jobs popover, the window-level
// shortcuts, drag-and-drop of files and the dock workspace (whose Picture panel shows the Welcome page while no file
// is open).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Item {
    id: shell

    // The central Actions object that the menu bar, the shortcuts and the panels share.
    readonly property alias actions: actions
    // The Open-as dialog, for tests.
    readonly property alias openAsDialogItem: openAsDialog
    // The Jobs popover, for tests.
    readonly property alias jobsPopoverItem: jobsPopover

    // Opens or closes the Jobs popover (toolbar indicator, Ctrl+J).
    function toggleJobs() {
        if (jobsPopover.opened)
            jobsPopover.close()
        else
            jobsPopover.open()
    }

    Actions {
        id: actions
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
                shell.toggleJobs()
        }
    }

    // Top-right corner of the shell, just below the toolbar: the shell's own top edge in overlay coordinates, taken
    // when the popover opens (the shell does not know which root hosts it).
    JobsPopover {
        id: jobsPopover

        parent: Overlay.overlay
        x: parent.width - width - OvtStyle.spacing
        onAboutToShow: y = shell.mapToItem(parent, 0, 0).y + OvtStyle.spacing
    }

    // GUI-JOB-0001 (§11.7): a job that did not stop in time is detached and reported.
    Connections {
        target: JobsModel
        function onJobDetached(name) {
            DiagnosticsModel.addGui(1, "GUI-JOB-0001", "job " + name + " did not stop within 2 s; detached")
        }
    }

    // The dock areas and panels (§11.2.2, WI-1.451). Focus panel N and tab cycling come in through actions.
    // The first-run banners (§11.8) sit above the dock areas; the Welcome page is the Picture panel's content while no
    // file is open (DockWorkspace).
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        FirstRunBanner {
            Layout.fillWidth: true
        }
        DockWorkspace {
            id: dockWorkspace

            Layout.fillWidth: true
            Layout.fillHeight: true
            actions: actions
            panelComponents: ({"diagnostics": diagnosticsComponent})
        }
    }

    // The Diagnostics / Log panel (§11.3.16) is a dock panel in the Bottom area; the status bar badge toggles it.
    Component {
        id: diagnosticsComponent

        DiagnosticsPanel {}
    }
}
