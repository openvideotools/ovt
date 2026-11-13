// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Unit list panel (chapter 11 §11.3.6, WI-1.459): the table of NAL units / OBUs in decode order with the filter chips
// (VCL, non-VCL, SEI, errors only, type text), sortable columns and the ⓗ toggle for Offset and Size. A click selects
// the unit, a double-click on a VCL unit seeks its frame. All filtering and sorting happens in UnitListModel; nothing
// is parsed here. Set `streamIndex` to the open file's StreamIndexModel.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Item {
    id: panel

    // The StreamIndexModel of the open file (null: empty panel).
    property var streamIndex: null
    readonly property alias units: units
    readonly property string panelId: "units"

    objectName: "unitListPanel"
    implicitHeight: 260
    Accessible.name: "Unit list"

    // Class tints of the Type column (§11.3.6), drawn over the panel colour at low alpha so the text keeps its contrast.
    readonly property var classTint: ({
            "vcl": Qt.rgba(0.5, 0.5, 0.5, 0.18),
            "parameterSet": Qt.rgba(0.12, 0.53, 0.9, 0.22),
            "sei": Qt.rgba(0.2, 0.7, 0.3, 0.22),
            "aud": Qt.rgba(0.5, 0.5, 0.5, 0.06),
            "other": Qt.rgba(0, 0, 0, 0)
        })
    readonly property var columnWidths: [64, 150, 56, 96, 90, 80, 64, 90]

    UnitListModel {
        id: units

        streamIndex: panel.streamIndex
        selection: Selection
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        RowLayout {
            Layout.fillWidth: true
            spacing: OvtStyle.spacing

            Repeater {
                model: ["All", "VCL", "Non-VCL", "SEI", "Errors only"]

                delegate: Button {
                    required property int index
                    required property string modelData

                    objectName: "chip" + modelData
                    text: modelData
                    checkable: true
                    autoExclusive: true
                    checked: units.kind === index
                    onClicked: units.kind = index
                    Accessible.name: modelData + " units"
                }
            }
            TextField {
                id: typeFilter

                objectName: "unitTypeFilter"
                Layout.fillWidth: true
                placeholderText: "Filter by type"
                onTextChanged: units.text = text
                Accessible.name: "Unit type filter"
            }
            CheckBox {
                objectName: "unitHex"
                text: "Hex"
                checked: units.hexNumbers
                onToggled: units.hexNumbers = checked
                Accessible.name: "Show offset and size in hexadecimal"
            }
            Label {
                text: units.rowCount + " / " + units.unitCount
                color: OvtStyle.textMuted
            }
        }

        HorizontalHeaderView {
            id: header

            Layout.fillWidth: true
            syncView: table
            clip: true
            model: ["#", "Type", "L/T", "Offset", "Size", "Frame", "Sample", "Flags"]
            delegate: ItemDelegate {
                required property int index
                required property string modelData

                implicitHeight: OvtStyle.minHitTarget
                text: modelData + (units.sortColumn === index ? (units.sortDescending ? " ▼" : " ▲") : "")
                onClicked: units.toggleSort(index)
                Accessible.name: "Sort by " + modelData
            }
        }

        Label {
            visible: units.unitCount === 0
            text: "No units"
            color: OvtStyle.textMuted
            Layout.alignment: Qt.AlignHCenter
        }

        TableView {
            id: table

            objectName: "unitTable"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            model: units
            columnWidthProvider: column => panel.columnWidths[column]
            rowHeightProvider: () => OvtStyle.minHitTarget
            ScrollBar.vertical: ScrollBar {
            }
            ScrollBar.horizontal: ScrollBar {
            }

            delegate: Rectangle {
                id: cellItem

                required property int row
                required property int column
                required property string display
                required property string unitClass
                required property int unitIndex

                implicitWidth: 80
                implicitHeight: OvtStyle.minHitTarget
                color: unitIndex === Selection.unitIndex ? OvtStyle.highlight : (column === 1 ? panel.classTint[unitClass] : "transparent")

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    anchors.rightMargin: 4
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: cellItem.column === 1 || cellItem.column === 7 ? Text.AlignLeft : Text.AlignRight
                    elide: Text.ElideRight
                    text: cellItem.display
                    color: cellItem.unitIndex === Selection.unitIndex ? OvtStyle.highlightedText : OvtStyle.text
                }
                TapHandler {
                    onTapped: units.select(cellItem.row, panel.panelId)
                    onDoubleTapped: units.activate(cellItem.row, panel.panelId)
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: rowMenu.openFor(cellItem.row)
                }
            }
        }
    }

    Menu {
        id: rowMenu

        property int row: -1

        function openFor(r) {
            row = r;
            popup();
        }

        MenuItem {
            text: "Copy row"
            onTriggered: {
                copyBuffer.text = units.rowText(rowMenu.row);
                copyBuffer.selectAll();
                copyBuffer.copy();
            }
        }
    }

    TextEdit {
        id: copyBuffer

        visible: false
    }

    // Ctrl+F focuses the type filter while the panel has focus (§11.3.6).
    Shortcut {
        sequence: StandardKey.Find
        enabled: panel.activeFocus || typeFilter.activeFocus
        onActivated: typeFilter.forceActiveFocus()
    }
}
