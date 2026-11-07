// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Timeline panel (chapter 11 §11.3.1; WI-1.455b): series and order controls, the TimelineChart and the range status
// line. The chart reports clicks; this file turns them into Selection changes (origin "timeline") and shows the
// selected frame back on the chart. The filmstrip, thumbnails and reference arrows follow in WI-1.455c.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Item {
    id: panel

    // The StreamIndexModel of the open file; the panel is empty without one.
    property StreamIndexModel index: null
    readonly property string panelId: "timeline"
    property alias chart: chart

    implicitWidth: 640
    implicitHeight: 220
    objectName: "timelinePanel"
    Accessible.role: Accessible.Pane
    Accessible.name: "Timeline"

    function rangeText() {
        const s = panel.index ? panel.index.rangeStats : ({})
        if (s.frames === undefined)
            return "No range selected: drag over the bars"
        const mb = (s.bytes / 1e6).toFixed(2)
        const mbit = (s.bitsPerSecond / 1e6).toFixed(1)
        const seconds = s.seconds.toFixed(2)
        return "range: " + panel.index.rangeFirst + "–" + panel.index.rangeLast + " · " + s.frames + " frames · "
                + seconds + " s · " + mb + " MB · " + mbit + " Mbit/s · avg QP " + s.meanQp.toFixed(1)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        RowLayout {
            Layout.fillWidth: true
            spacing: OvtStyle.spacing

            Label {
                text: "Series"
                color: OvtStyle.text
            }
            ComboBox {
                id: seriesBox

                objectName: "seriesBox"
                model: ["Bits", "QP", "Temporal id"]
                Accessible.name: "Series"
                onActivated: chart.series = currentIndex
            }
            CheckBox {
                text: "Mean line"
                checked: chart.meanLine
                onToggled: chart.meanLine = checked
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                text: "Order"
                color: OvtStyle.text
            }
            ComboBox {
                id: orderBox

                objectName: "orderBox"
                model: ["Decode", "Display"]
                currentIndex: panel.index && panel.index.displayOrder ? 1 : 0
                Accessible.name: "Frame order"
                onActivated: {
                    panel.index.displayOrder = currentIndex === 1
                    Selection.selectDisplayOrder(currentIndex === 1, panel.panelId)
                }
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

            TimelineChart {
                id: chart

                objectName: "timelineChart"
                anchors.fill: parent
                anchors.margins: 1
                model: panel.index
                selectedFrame: Selection.frameDecodeIndex
                activeFocusOnTab: true
                Accessible.role: Accessible.Chart
                Accessible.name: "Frame timeline"

                onFrameClicked: (decodeIndex, displayIndex) => Selection.selectFrame(decodeIndex, displayIndex,
                                                                                       panel.panelId)
                onStepRequested: delta => {
                    // From the selected frame, or the first frame when none is selected.
                    const current = panel.index.positionOf(Selection.frameDecodeIndex)
                    const target = Math.max(0, Math.min(panel.index.frameCount - 1, current < 0 ? 0 : current + delta))
                    const decode = panel.index.decodeIndexAt(target)
                    Selection.selectFrame(decode, panel.index.displayIndexOf(decode), panel.panelId)
                }
                onHovered: (position, x) => {
                    hoverTip.text = position >= 0 ? chart.frameTooltip(position) : ""
                    hoverTip.x = Math.min(x + 12, chart.width - hoverTip.width)
                }
                HoverHandler {
                    id: hoverHandler
                }
            }

            ToolTip {
                id: hoverTip

                parent: chart
                y: 8
                visible: hoverHandler.hovered && text !== ""
                delay: 300
            }

            Label {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 4
                text: chart.maxValue > 0 ? Math.round(chart.maxValue).toString() : ""
                color: OvtStyle.textMuted
                font.pixelSize: OvtStyle.fontSize - 2
            }
        }

        Label {
            objectName: "rangeLabel"
            Layout.fillWidth: true
            text: panel.rangeText()
            color: OvtStyle.text
            elide: Text.ElideRight
        }
    }
}
