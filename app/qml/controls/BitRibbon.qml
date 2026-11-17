// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bit ribbon of the Hex panel (chapter 11 §11.3.5, WI-1.458): the bytes around the start of the selected field in
// binary, with the exact bit range of the field boxed and the field's name. `bytes` is HexModel.ribbon: a list of
// {offset, value, bits, mask, epb}; `mask` has a bit set (MSB = bit 0 of the byte) for every bit inside the field.
// Emulation-prevention bytes are dimmed with a strikethrough, as in the table.

import QtQuick
import QtQuick.Layouts
import OvtStudio

Item {
    id: ribbon

    property var bytes: []
    property string fieldName: ""
    readonly property int bitWidth: 9
    readonly property int cellHeight: 18

    objectName: "bitRibbon"
    implicitHeight: cellHeight * 2 + OvtStyle.spacing * 2
    Accessible.name: "Bit ribbon"
    Accessible.description: fieldName

    function covered(mask, bit) {
        return ((mask >> (7 - bit)) & 1) === 1
    }
    function firstBit(mask) {
        for (let i = 0; i < 8; ++i)
            if (covered(mask, i))
                return i
        return -1
    }
    function lastBit(mask) {
        for (let i = 7; i >= 0; --i)
            if (covered(mask, i))
                return i
        return -1
    }

    Column {
        anchors.fill: parent
        anchors.margins: OvtStyle.spacing
        spacing: 2

        Text {
            objectName: "ribbonTitle"
            text: ribbon.fieldName.length > 0 ? ribbon.fieldName : qsTr("bit ribbon")
            color: OvtStyle.textMuted
            font.pixelSize: OvtStyle.fontSize
            height: ribbon.cellHeight
        }
        Row {
            spacing: OvtStyle.spacing * 2

            Repeater {
                model: ribbon.bytes

                delegate: Item {
                    id: byteItem

                    required property var modelData

                    objectName: "ribbonByte"
                    width: ribbon.bitWidth * 8 + 4
                    height: ribbon.cellHeight
                    opacity: modelData.epb ? 0.45 : 1

                    Rectangle {
                        // The box around the covered bits of this byte.
                        visible: ribbon.firstBit(byteItem.modelData.mask) >= 0
                        x: ribbon.firstBit(byteItem.modelData.mask) * ribbon.bitWidth + 1
                        width: (ribbon.lastBit(byteItem.modelData.mask) - ribbon.firstBit(byteItem.modelData.mask) + 1)
                               * ribbon.bitWidth + 2
                        height: parent.height
                        color: Qt.rgba(1, 0.84, 0, 0.45)
                        border.color: "#FFD700"
                        border.width: 1
                    }
                    Text {
                        x: 2
                        anchors.verticalCenter: parent.verticalCenter
                        text: byteItem.modelData.bits
                        color: OvtStyle.text
                        font.family: "monospace"
                        font.pixelSize: OvtStyle.fontSize
                        font.strikeout: byteItem.modelData.epb
                    }
                }
            }
        }
    }
}
