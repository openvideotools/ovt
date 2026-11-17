// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The root item that MainActivity's QtQuickView loads on Android (chapter 13 §13.4.3; WI-0.050, WI-A.450).
// QtQuickView shows an Item inside the Activity and cannot take a Window, so the desktop Main.qml (an
// ApplicationWindow) is not loaded here. SPEC-ASSUMPTION(#356): WI-A.450 brings the desktop shell into the Activity
// by hosting the same pieces Main.qml hosts (OvtMenuBar, MainToolBar, AppShell, StatusBar) in a Page, an Item that
// carries the window palette and font as ApplicationWindow does on the desktop. The Welcome page (WI-1.464) is
// therefore what a fresh launch shows (WI-A.450 acceptance test 1). The breakpoint layouts of §11.5.2 are WI-A.451.
// Qt 6.8 does not expose the Qt Quick accessibility tree through QtQuickView (issue #547), so MainActivity reads
// `productName` once the root is ready and puts it on the Qt view's own accessibility node, which UI Automator finds
// (WI-0.050 acceptance test 2).
//
// WI-0.052 painted only a header band so that the HDR test pattern of HdrSurfaceView (below QtQuickView, chapter 13
// §13.4.4 item 1) showed through; the full shell is opaque and covers that surface. WI-A.400 cuts the Picture
// panel's hole and replaces MainActivity.SHELL_HEADER_FRACTION with the panel's geometry.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Page {
    id: root

    // The shell, for tests.
    readonly property alias shellItem: shell
    // The product name of the application, read by MainActivity through QtQuickView.getProperty() (issue #547).
    readonly property string productName: Qt.application.displayName
    // The Activity's Configuration.screenWidthDp, set by MainActivity.onConfigurationChanged through
    // QtQuickView.setProperty() (WI-A.451, §11.5.2): the window width the breakpoint layouts follow after start-up.
    // 0 until the first change; Platform reads the start-up width itself.
    property int configurationWidthDp: 0
    onConfigurationWidthDpChanged: {
        if (configurationWidthDp > 0) {
            console.log("OVT: configuration widthDp=" + configurationWidthDp)
            Platform.refresh(configurationWidthDp)
        }
    }

    // §11.5.7 (WI-A.455): the GUI state MainActivity keeps in its saved instance state. Each QtQuickView builds its
    // own QML engine, so a recreated Activity ("Don't keep activities", process death) starts with fresh models;
    // OvtSession keeps only the core handles. The state travels as JSON (AndroidState, ≤ 100 KB):
    //  - uiStateSaved(json) is emitted after every change; MainActivity keeps the latest one (QtQuickView signal
    //    listener) and puts it into the Bundle in onSaveInstanceState, so saving never waits on the Qt thread.
    //  - MainActivity sets restoredState once the root is READY: the saved JSON, or "{}" when there is none. Nothing
    //    is published before that, so the defaults of a fresh engine never replace the state still to be restored.
    signal uiStateSaved(string json)
    property string restoredState: ""
    property bool stateReceived: false
    property string lastSavedState: ""
    readonly property alias uiState: uiState

    onRestoredStateChanged: {
        if (restoredState.length === 0)
            return
        stateReceived = true
        if (!uiState.picture)
            uiState.picture = pictureController()
        if (restoredState !== "{}") {
            const ok = uiState.restore(restoredState)
            console.log("OVT: state restored=" + ok + " frame=" + Selection.frameDecodeIndex)
        }
        publishState()
    }

    function publishState() {
        if (!stateReceived)
            return
        if (!uiState.picture)
            uiState.picture = pictureController()
        const json = uiState.capture()
        if (json.length > 0 && json !== lastSavedState) {
            lastSavedState = json
            root.uiStateSaved(json)
        }
    }

    // The Picture panel's controller (the ViewState owner), once the panel exists.
    function pictureController() {
        const item = shell.dockWorkspace ? shell.dockWorkspace.panelItems["picture"] : undefined
        return item && item.controller ? item.controller : null
    }

    AndroidState {
        id: uiState

        selection: Selection
        dock: DockModel
        project: Project
        // Shell state the models do not hold.
        panelState: shell.dockWorkspace ? {
            "focusedPanel": shell.dockWorkspace.focusedPanel,
            "drawerOpen": shell.dockWorkspace.drawerOpen,
            "singleIndex": shell.dockWorkspace.singleIndex
        } : ({})
        onPanelStateChanged: Qt.callLater(root.publishState)
        onRestored: {
            const ws = shell.dockWorkspace
            if (!ws)
                return
            const p = panelState
            if (typeof p.singleIndex === "number")
                ws.singleIndex = p.singleIndex
            if (typeof p.drawerOpen === "boolean")
                ws.drawerOpen = p.drawerOpen
            if (typeof p.focusedPanel === "string" && p.focusedPanel.length > 0)
                ws.focusedPanel = p.focusedPanel
        }
    }

    // Changes that alter the saved state; one capture per event-loop turn.
    Connections {
        target: Selection
        function onChanged(mask) { Qt.callLater(root.publishState) }
    }
    Connections {
        target: DockModel
        function onLayoutChanged() { Qt.callLater(root.publishState) }
    }
    Connections {
        target: Project
        function onFilesChanged() { Qt.callLater(root.publishState) }
        function onPathChanged() { Qt.callLater(root.publishState) }
    }
    Connections {
        target: uiState.picture
        ignoreUnknownSignals: true
        function onViewChanged() { Qt.callLater(root.publishState) }
        function onPlaneChanged() { Qt.callLater(root.publishState) }
    }

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

    // As Main.qml does on the desktop: bookmarks follow the open project (WI-2.487). The Picture panel's controller
    // joins the saved state (it is created with the dock, before this runs).
    Component.onCompleted: {
        Bookmarks.attach(Project, Selection)
        uiState.picture = root.pictureController()
    }
}
