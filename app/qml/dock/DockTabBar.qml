// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tab headers of a group (chapter 11 §11.2.2; WI-1.451). Click selects; dragging a header shows the drop
// targets and moves the panel on release; the context menu has Detach (desktop) and Close (§11.3 rule a).

import QtQuick
import QtQuick.Controls
import OvtStudio

Rectangle {
    id: bar

    required property Item group

    implicitHeight: Math.max(28, OvtStyle.minHitTarget)
    color: OvtStyle.window

    Row {
        anchors.fill: parent
        spacing: 1

        Repeater {
            model: bar.group.panelIds
            delegate: Rectangle {
                id: tab

                required property string modelData
                required property int index
                readonly property bool current: index === bar.group.currentIndex

                objectName: "dockTab_" + modelData
                width: label.implicitWidth + 2 * OvtStyle.spacing + 4
                height: bar.height
                color: current ? OvtStyle.panel : OvtStyle.window
                opacity: drag.active ? 0.5 : 1
                Accessible.role: Accessible.PageTab
                Accessible.name: label.text

                Label {
                    id: label

                    anchors.centerIn: parent
                    text: DockModel.panelTitle(tab.modelData)
                    color: tab.current ? OvtStyle.text : OvtStyle.textMuted
                    font.bold: tab.current
                }

                TapHandler {
                    onTapped: {
                        DockModel.setCurrentIndex(bar.group.groupIndex, tab.index)
                        bar.group.workspace.focusedPanel = tab.modelData
                    }
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: menu.popup()
                }
                DragHandler {
                    id: drag

                    target: null
                    onActiveChanged: {
                        const indicator = bar.group.workspace.dropIndicator
                        if (active)
                            indicator.begin(tab.modelData, centroid.scenePosition)
                        else
                            indicator.finish()
                    }
                    onCentroidChanged: if (active) bar.group.workspace.dropIndicator.move(centroid.scenePosition)
                }

                Menu {
                    id: menu

                    MenuItem {
                        text: qsTr("Detach")
                        enabled: DockModel.floatingSupported && tab.modelData !== "picture"
                        onTriggered: DockModel.floatPanel(tab.modelData)
                    }
                    MenuItem {
                        text: qsTr("Close")
                        enabled: tab.modelData !== "picture"
                        onTriggered: DockModel.closePanel(tab.modelData)
                    }
                }
            }
        }
    }
}
