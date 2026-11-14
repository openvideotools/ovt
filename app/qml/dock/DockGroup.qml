// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A tab group (chapter 11 §11.2.2; WI-1.451): a tab bar over the current panel. The panel items belong to the
// workspace; the group only re-parents the ones it shows, so a panel survives being moved to another group.

import QtQuick
import QtQuick.Controls
import OvtStudio

Rectangle {
    id: group

    required property Item workspace
    required property string areaName
    property int areaPosition: 0
    property int groupIndex: -1
    property var panelIds: []
    property int currentIndex: 0

    color: OvtStyle.panel
    border.color: OvtStyle.border

    function place() {
        for (let i = 0; i < panelIds.length; ++i) {
            const item = workspace.panelItem(panelIds[i])
            item.parent = content
            item.anchors.fill = content
            item.visible = i === currentIndex
        }
    }

    onPanelIdsChanged: place()
    onCurrentIndexChanged: place()
    Component.onCompleted: {
        workspace.registerGroup(group)
        place()
    }
    Component.onDestruction: workspace.unregisterGroup(group)

    DockTabBar {
        id: tabBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        group: group
    }

    Item {
        id: content

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tabBar.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 1
        clip: true
    }
}
