// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One dock area (chapter 11 §11.2.2; WI-1.451): Left and Right stack their groups vertically, Bottom horizontally.
// The groups come from DockModel; their sizes follow `sizeFraction` and are written back when a splitter drag ends.

import QtQuick
import QtQuick.Controls
import OvtStudio

SplitView {
    id: area

    required property string areaName
    required property Item workspace
    // A string, so that the groups are only rebuilt when this area changes (it notifies on a new value only).
    readonly property string groupsJson: JSON.stringify(DockModel.areas[areaName])
    readonly property var groupList: JSON.parse(groupsJson)
    readonly property int groupCount: groupList.length
    readonly property bool horizontal: areaName === "bottom"

    orientation: horizontal ? Qt.Horizontal : Qt.Vertical
    handle: Rectangle {
        implicitWidth: area.workspace.handleSize
        implicitHeight: area.workspace.handleSize
        color: SplitHandle.pressed ? OvtStyle.accent : SplitHandle.hovered ? OvtStyle.border : OvtStyle.window
        TapHandler {
            onDoubleTapped: DockModel.resetSizes()
        }
    }

    onResizingChanged: {
        if (resizing)
            return
        const sizes = []
        for (let i = 0; i < groupCount; ++i) {
            const item = itemAt(i)
            sizes.push(horizontal ? item.width : item.height)
        }
        DockModel.setSizeFractions(areaName, sizes)
    }

    Repeater {
        model: area.groupList
        delegate: DockGroup {
            required property var modelData
            required property int index

            objectName: "dockGroup_" + area.areaName + "_" + index
            workspace: area.workspace
            areaName: area.areaName
            areaPosition: index
            groupIndex: modelData.group
            panelIds: modelData.panelIds
            currentIndex: modelData.currentIndex
            SplitView.preferredWidth: area.horizontal ? area.width * modelData.sizeFraction : -1
            SplitView.preferredHeight: area.horizontal ? -1 : area.height * modelData.sizeFraction
            SplitView.minimumWidth: 60
            SplitView.minimumHeight: 60
        }
    }
}
