// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference / GOP graph panel (chapter 11 §11.3.11; WI-2.484): the RefGraph item, its row and order controls and the
// DPB strip. The item reports clicks; this file turns them into Selection changes (origin "ref_graph") and shows the
// selected frame back on the graph. References come from the index headers (`declared_refs`); the "used" arrow mode
// needs FrameAnalysis.refs and stays disabled until the analysis path is wired.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Item {
    id: panel

    // The StreamIndexModel of the open file; the panel is empty without one.
    property StreamIndexModel index: null
    readonly property string panelId: "ref_graph"
    property alias graph: graph

    implicitWidth: 640
    implicitHeight: 240
    objectName: "refGraphPanel"
    Accessible.role: Accessible.Pane
    Accessible.name: "Reference graph"

    function dpbText() {
        if (!panel.index || Selection.frameDecodeIndex < 0)
            return "DPB: select a frame"
        if (!graph.dpbKnown)
            return "DPB: not derivable for this codec/plugin"
        const chips = graph.dpbChips(Selection.frameDecodeIndex)
        return "DPB after #" + Selection.frameDecodeIndex + ": " + chips.length + " frame" + (chips.length === 1 ? "" : "s")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        RowLayout {
            Layout.fillWidth: true
            spacing: OvtStyle.spacing

            Label {
                text: "Rows"
                color: OvtStyle.text
            }
            ComboBox {
                id: rowBox

                objectName: "rowModeBox"
                model: ["Temporal layers", "Decode vs display", "Flat"]
                Accessible.name: "Row mode"
                onActivated: graph.rowMode = currentIndex
            }
            Label {
                text: "Order"
                color: OvtStyle.text
            }
            ComboBox {
                id: axisBox

                objectName: "axisBox"
                model: ["Decode", "Display"]
                currentIndex: 1
                enabled: graph.rowMode !== 1
                Accessible.name: "Frame order"
                onActivated: graph.displayAxis = currentIndex === 1
            }
            Label {
                text: "Arrows"
                color: OvtStyle.text
            }
            Label {
                objectName: "arrowSource"
                // "Used" arrows need FrameAnalysis.refs, which the panel does not receive yet.
                text: "declared"
                color: OvtStyle.textMuted
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                text: "window ±64 frames · wheel scrolls"
                color: OvtStyle.textMuted
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: OvtStyle.panel
            border.color: OvtStyle.border

            Label {
                anchors.centerIn: parent
                visible: !panel.index || panel.index.frameCount === 0
                text: "No file open"
                color: OvtStyle.textMuted
            }

            RefGraph {
                id: graph

                objectName: "refGraph"
                anchors.fill: parent
                anchors.margins: 1
                model: panel.index
                selectedFrame: Selection.frameDecodeIndex
                textColor: OvtStyle.text
                outlineColor: OvtStyle.text
                Accessible.role: Accessible.Chart
                Accessible.name: "Reference graph"

                onFrameClicked: (decodeIndex, displayIndex) => Selection.selectFrame(decodeIndex, displayIndex,
                                                                                       panel.panelId)

                ToolTip {
                    id: tip

                    visible: false
                }
                onFrameHovered: decodeIndex => {
                    if (decodeIndex < 0) {
                        tip.visible = false
                        return
                    }
                    tip.text = graph.frameTooltip(decodeIndex)
                    tip.visible = true
                }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: contextMenu.popup()
                }
                Menu {
                    id: contextMenu

                    MenuItem {
                        text: "Copy graph as SVG"
                        onTriggered: graph.copySvg()
                    }
                    MenuItem {
                        text: "Copy frame list"
                        onTriggered: graph.copyFrameList()
                    }
                }
            }
        }

        // The DPB strip: the frames in the decoded picture buffer after the selected frame; a chip selects its frame.
        RowLayout {
            Layout.fillWidth: true
            spacing: OvtStyle.spacing

            Label {
                objectName: "dpbLabel"
                text: panel.dpbText()
                color: OvtStyle.text
            }
            Repeater {
                model: graph.dpbKnown && panel.index ? graph.dpbChips(Selection.frameDecodeIndex) : []

                delegate: Button {
                    required property var modelData

                    objectName: "dpbChip"
                    text: modelData.label
                    onClicked: Selection.selectFrame(modelData.decodeIndex, panel.index.displayIndexOf(modelData.decodeIndex),
                                                     panel.panelId)
                }
            }
            Item {
                Layout.fillWidth: true
            }
        }
    }
}
