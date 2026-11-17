// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hex panel (chapter 11 §11.3.5, WI-1.458): 16 bytes per row with offset, hex and ASCII columns, the unit / box
// bands, EPB bytes dimmed and struck through, the selected field's bits highlighted (partial bytes striped), the bit
// ribbon (Ctrl+B), Go to (0x…, decimal, unit:N, frame:N), the byte search with `??` wildcards (F3 / Shift+F3 for the
// next / previous match) and the scope combo. All of it is computed in HexModel; this file only draws.
// `Ctrl+B` is a panel key: `B` stays the window's Add bookmark and is never taken here.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Item {
    id: panel

    // The StreamIndexModel of the open file (null: no bands, no EPB marks).
    property var streamIndex: null
    // The shell's Actions (Ctrl+F focuses the search field); optional.
    property var shellActions: null
    // Set by the dock workspace when it creates the panel item (it passes {"panelId": id}).
    property string panelId: "hex"
    readonly property alias hex: hex
    readonly property alias table: table

    objectName: "hexPanel"
    implicitHeight: 300
    Accessible.name: "Hex"

    readonly property real rowHeight: 18
    readonly property var columnWidths: [78, 22, 22, 22, 22, 22, 22, 22, 22, 28, 22, 22, 22, 22, 22, 22, 22, 150]
    readonly property color gold: "#FFD700"
    // §11.3.5: #1E88E5 at 12 % / 8 %, the prefix darker; boxes use the same scheme with darker headers.
    readonly property var bandColor: [
        "transparent", Qt.rgba(0.118, 0.533, 0.898, 0.12), Qt.rgba(0.118, 0.533, 0.898, 0.08),
        Qt.rgba(0.118, 0.533, 0.898, 0.28), Qt.rgba(0.2, 0.7, 0.4, 0.12), Qt.rgba(0.2, 0.7, 0.4, 0.08),
        Qt.rgba(0.2, 0.7, 0.4, 0.28)
    ]

    HexModel {
        id: hex

        streamIndex: panel.streamIndex
        selection: Selection
        jobs: JobsModel

        onRevealRequested: function (row) {
            // Keep the row in view, centred when it was off screen.
            if (row < table.topRow || row > table.bottomRow)
                table.positionViewAtRow(Math.max(0, row - 2), Qt.AlignTop)
        }
    }

    Connections {
        target: panel.shellActions
        ignoreUnknownSignals: true
        function onActivated(id) {
            if (id === "edit.find" && panel.activeFocus)
                findField.forceActiveFocus()
        }
    }

    function status() {
        if (!hex.hasSource)
            return ""
        if (hex.searching)
            return qsTr("Searching…")
        if (hex.matchCount > 0)
            return qsTr("%1 of %2").arg(Math.max(hex.currentMatch + 1, 0)).arg(hex.matchCount)
                   + (hex.truncated ? "+" : "")
        return findField.text.length > 0 && findField.searched ? qsTr("No match") : ""
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: OvtStyle.spacing
            spacing: OvtStyle.spacing
            visible: hex.hasSource

            TextField {
                id: gotoField

                objectName: "hexGoto"
                placeholderText: qsTr("goto: 0x1A3F40")
                Layout.preferredWidth: 150
                color: gotoError.length > 0 ? OvtStyle.error : OvtStyle.text
                property string gotoError: ""
                onTextChanged: gotoError = ""
                onAccepted: {
                    gotoError = hex.goTo(text)
                    if (gotoError.length === 0)
                        table.forceActiveFocus()
                }
                Accessible.name: "Go to offset, unit or frame"
                ToolTip.visible: gotoError.length > 0
                ToolTip.text: gotoError
            }
            TextField {
                id: findField

                objectName: "hexFind"
                placeholderText: qsTr("find bytes: 00 00 01")
                Layout.preferredWidth: 190
                property bool searched: false
                property string findError: ""
                color: findError.length > 0 ? OvtStyle.error : OvtStyle.text
                onTextChanged: {
                    searched = false
                    findError = ""
                }
                onAccepted: {
                    findError = hex.search(text)
                    searched = findError.length === 0
                }
                Accessible.name: "Find bytes"
                ToolTip.visible: findError.length > 0
                ToolTip.text: findError
            }
            Button {
                objectName: "hexPrev"
                text: "◀"
                enabled: hex.matchCount > 0
                onClicked: hex.findPrevious()
                Accessible.name: "Previous match"
            }
            Button {
                objectName: "hexNext"
                text: "▶"
                enabled: hex.matchCount > 0
                onClicked: hex.findNext()
                Accessible.name: "Next match"
            }
            Label {
                objectName: "hexSearchStatus"
                text: panel.status()
                color: OvtStyle.textMuted
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                text: qsTr("scope")
                color: OvtStyle.textMuted
            }
            ComboBox {
                objectName: "hexScope"
                model: [qsTr("File"), qsTr("Unit"), qsTr("Box")]
                currentIndex: hex.scope
                onActivated: function (index) {
                    hex.scope = index
                }
                Accessible.name: "Scope"
            }
        }

        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !hex.hasSource
            text: textFor("hex")
        }

        TableView {
            id: table

            objectName: "hexTable"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: hex.hasSource
            clip: true
            reuseItems: true
            focus: true
            activeFocusOnTab: true
            boundsBehavior: Flickable.StopAtBounds
            model: hex
            columnWidthProvider: column => panel.columnWidths[column]
            rowHeightProvider: () => panel.rowHeight
            ScrollBar.vertical: ScrollBar {
            }

            property real anchorOffset: -1

            // F3 / Shift+F3 are panel-local here (§11.4.3 precedence) and window-global (Motion vectors) elsewhere: the
            // table claims them so that the window shortcut does not fire while it has focus.
            Keys.onShortcutOverride: function (event) {
                if (event.key === Qt.Key_F3)
                    event.accepted = true
            }

            // Panel keys. `B` (Add bookmark) is a window shortcut and is left alone: nothing here accepts it.
            Keys.onPressed: function (event) {
                if (Keymap.matches("action.hex.binary", event.key, event.modifiers)) {
                    hex.showRibbon = !hex.showRibbon
                    event.accepted = true
                } else if (event.key === Qt.Key_F3) {
                    event.accepted = (event.modifiers & Qt.ShiftModifier) ? hex.findPrevious() : hex.findNext()
                }
            }

            delegate: Rectangle {
                id: cell

                required property int row
                required property int column
                required property string display
                required property int band
                required property bool epb
                required property int highlight
                required property bool loaded
                required property bool selected

                readonly property bool isByte: column >= 1 && column <= 16
                readonly property bool partial: highlight > 0 && highlight < 255

                implicitWidth: panel.columnWidths[column]
                implicitHeight: panel.rowHeight
                color: !loaded ? Qt.rgba(0.5, 0.5, 0.5, 0.12) : (isByte ? panel.bandColor[band] : "transparent")

                Rectangle {
                    // The selected field's bits: solid for whole bytes, a lighter fill with a rule for partial ones.
                    visible: cell.isByte && cell.highlight > 0
                    anchors.fill: parent
                    color: Qt.rgba(1, 0.84, 0, cell.partial ? 0.22 : 0.45)

                    Rectangle {
                        visible: cell.partial
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 2
                        color: panel.gold
                    }
                }
                Rectangle {
                    visible: cell.isByte && cell.selected
                    anchors.fill: parent
                    color: "transparent"
                    border.color: OvtStyle.accent
                    border.width: 1
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: column === 17 ? 8 : (cell.isByte ? (parent.width - width) / 2 : 4)
                    text: cell.loaded ? cell.display : "··"
                    color: column === 0 || column === 17 ? OvtStyle.textMuted : OvtStyle.text
                    opacity: cell.epb ? 0.45 : 1
                    font.family: "monospace"
                    font.pixelSize: OvtStyle.fontSize
                    font.strikeout: cell.epb
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: cell.isByte
                    acceptedButtons: Qt.LeftButton
                    onPressed: function (mouse) {
                        table.forceActiveFocus()
                        const offset = hex.offsetOfRow(cell.row) + cell.column - 1
                        if (mouse.modifiers & Qt.ShiftModifier && table.anchorOffset >= 0) {
                            hex.selectByteRange(table.anchorOffset, offset)
                        } else {
                            table.anchorOffset = offset
                            hex.selectByteRange(offset, offset)
                        }
                    }
                    onPositionChanged: function (mouse) {
                        if (!pressed)
                            return
                        const p = mapToItem(table.contentItem, mouse.x, mouse.y)
                        const hit = table.cellAtPosition(p.x, p.y, true)
                        if (hit.y >= 0 && hit.x >= 1 && hit.x <= 16 && table.anchorOffset >= 0)
                            hex.selectByteRange(table.anchorOffset, hex.offsetOfRow(hit.y) + hit.x - 1)
                    }
                }
            }
        }

        BitRibbon {
            Layout.fillWidth: true
            visible: hex.hasSource && hex.showRibbon
            bytes: hex.ribbon
            fieldName: hex.ribbonField
        }
    }
}
