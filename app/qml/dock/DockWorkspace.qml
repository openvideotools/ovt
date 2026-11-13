// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dock workspace of the main window (chapter 11 §11.2.2; WI-1.451): Left | (Center over Bottom) | Right, each
// area a SplitView of DockGroups, plus floating windows on the desktop, the drop indicator and the dock shortcuts.
// Panel items live here, one per open panel, and are re-parented into whichever group shows them, so moving a tab
// keeps the panel (and its state). Later work items register their panels in `panelComponents` (id → Component).

import QtQuick
import QtQuick.Controls
import OvtStudio

Item {
    id: workspace

    objectName: "dockWorkspace"

    property var panelComponents: ({})
    // The shell's Actions (activated(id) signal); null in tests that build the workspace alone.
    property var actions: null
    // The panel that Ctrl+Tab cycles from (the last one focused or clicked).
    property string focusedPanel: "picture"
    readonly property alias dropIndicator: dropIndicator
    readonly property real handleSize: Platform.isAndroid ? 12 : 6 // §11.2.2: 6 dip, 12 dip touch area

    property var panelItems: ({})
    property var groupItems: []

    function registerGroup(group) {
        groupItems.push(group)
    }
    function unregisterGroup(group) {
        const i = groupItems.indexOf(group)
        if (i >= 0)
            groupItems.splice(i, 1)
    }

    // The item of an open panel, created on first use.
    function panelItem(id) {
        let item = panelItems[id]
        if (!item) {
            const component = panelComponents[id] ?? placeholder
            item = component.createObject(parking, {"panelId": id})
            panelItems[id] = item
        }
        return item
    }

    function focusPanel(id) {
        if (!DockModel.setCurrentPanel(id))
            return
        focusedPanel = id
        panelItem(id).forceActiveFocus()
    }

    // Closed panels are destroyed (their state goes with them, §11.2.2 keeps it in panel_state once projects exist).
    function prune() {
        const open = DockModel.openPanels
        for (const id of Object.keys(panelItems)) {
            if (open.indexOf(id) < 0) {
                panelItems[id].destroy()
                delete panelItems[id]
            }
        }
        if (open.indexOf(focusedPanel) < 0)
            focusedPanel = "picture"
    }

    function applySizes() {
        leftArea.SplitView.preferredWidth = width * DockModel.leftSize
        rightArea.SplitView.preferredWidth = width * DockModel.rightSize
        bottomArea.SplitView.preferredHeight = column.height * DockModel.bottomSize
    }

    Connections {
        target: DockModel
        function onLayoutChanged() {
            workspace.prune()
            workspace.applySizes()
        }
    }
    onWidthChanged: applySizes()

    // Holds panel items that no group shows at the moment.
    Item {
        id: parking

        visible: false
    }

    Component {
        id: placeholder

        Rectangle {
            id: panel

            required property string panelId
            // Panel is the panel's title for the not-yet-built ones; the empty states are §11.8's sentences.
            readonly property string emptyText: emptyState.textFor(panelId)

            color: OvtStyle.panel
            Accessible.name: DockModel.panelTitle(panelId)

            // The Picture area shows the Welcome page while no file is open (§11.8).
            WelcomePage {
                anchors.fill: parent
                visible: panel.panelId === "picture" && Project.fileCount === 0
                actions: workspace.actions
            }
            EmptyState {
                id: emptyState

                anchors.fill: parent
                visible: !(panel.panelId === "picture" && Project.fileCount === 0)
                text: panel.emptyText !== "" ? panel.emptyText : qsTr("%1 (not available yet)").arg(DockModel.panelTitle(panel.panelId))
            }
        }
    }

    component Handle: Rectangle {
        implicitWidth: workspace.handleSize
        implicitHeight: workspace.handleSize
        color: SplitHandle.pressed ? OvtStyle.accent : SplitHandle.hovered ? OvtStyle.border : OvtStyle.window
        TapHandler {
            onDoubleTapped: DockModel.resetSizes()
        }
    }

    // Writes the area sizes back once a splitter drag ends.
    function storeAreaSizes() {
        if (width > 0 && column.height > 0) {
            DockModel.leftSize = leftArea.visible ? leftArea.width / width : DockModel.leftSize
            DockModel.rightSize = rightArea.visible ? rightArea.width / width : DockModel.rightSize
            DockModel.bottomSize = bottomArea.visible ? bottomArea.height / column.height : DockModel.bottomSize
        }
    }

    SplitView {
        id: row

        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: Handle {}
        onResizingChanged: if (!resizing) workspace.storeAreaSizes()

        DockArea {
            id: leftArea

            objectName: "dockArea_left"
            areaName: "left"
            workspace: workspace
            visible: DockModel.leftVisible && !DockModel.pictureOnly && groupCount > 0
            SplitView.minimumWidth: 120
        }

        SplitView {
            id: column

            orientation: Qt.Vertical
            SplitView.fillWidth: true
            onHeightChanged: workspace.applySizes()
            handle: Handle {}
            onResizingChanged: if (!resizing) workspace.storeAreaSizes()

            DockGroup {
                id: centerGroup

                readonly property var info: DockModel.areas.center[0]
                objectName: "dockGroup_center"
                workspace: workspace
                areaName: "center"
                groupIndex: info.group
                panelIds: info.panelIds
                currentIndex: info.currentIndex
                SplitView.fillHeight: true
            }

            DockArea {
                id: bottomArea

                objectName: "dockArea_bottom"
                areaName: "bottom"
                workspace: workspace
                visible: DockModel.bottomVisible && !DockModel.pictureOnly && groupCount > 0
                SplitView.minimumHeight: 80
            }
        }

        DockArea {
            id: rightArea

            objectName: "dockArea_right"
            areaName: "right"
            workspace: workspace
            visible: DockModel.rightVisible && !DockModel.pictureOnly && groupCount > 0
            SplitView.minimumWidth: 120
        }
    }

    // Floating windows (desktop only, §11.2.1): the file is not even part of the Android build.
    readonly property string floatingJson: DockModel.floatingSupported ? JSON.stringify(DockModel.areas.floating) : "[]"
    Repeater {
        model: JSON.parse(workspace.floatingJson)
        delegate: Loader {
            required property var modelData

            source: Qt.resolvedUrl("DockFloatingWindow.qml")
            onLoaded: item.setup(modelData, workspace)
        }
    }

    DockDropIndicator {
        id: dropIndicator

        anchors.fill: parent
        workspace: workspace
    }

    // Keyboard (§11.2.2): the keys come from the Keymap through the shortcut layer (WI-1.466); Actions handles the
    // model toggles (Picture only, areas) and this item the ones that need the panel items.
    Connections {
        target: workspace.actions
        enabled: workspace.actions !== null

        function onActivated(id) {
            if (id === "view.nextTab" || id === "view.previousTab") {
                workspace.focusPanel(DockModel.cycleTab(workspace.focusedPanel, id === "view.nextTab" ? 1 : -1))
            } else if (id.indexOf("view.focusPanel") === 0) {
                const n = Number(id.substring("view.focusPanel".length)) - 1
                const order = DockModel.tabOrder()
                if (n >= 0 && n < order.length)
                    workspace.focusPanel(order[n])
            }
        }
    }
}
