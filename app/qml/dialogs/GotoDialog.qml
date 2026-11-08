// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Go to (Ctrl+G, FR-GUI-006, chapter 11 §11.4.3): one text field that takes a decode index (d123), display index (123,
// p123), POC (poc45), time (12.5s, 00:01:30.250), byte offset (@0x1A2B3C, @1715004) or unit (unit:12). The text is
// parsed by Goto.parse; the panels do the navigating through `handler` (the Actions.goToHandler): it returns "" when
// it moved or the reason it did not, and the dialog then stays open showing the reason, with the current frame
// unchanged. Without a handler (no file open, no panel to scroll) the answer is "no frame".

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Dialog {
    id: dialog

    objectName: "gotoDialog"
    property var handler: null
    // The last parsed target, for tests and callers that do not use a handler.
    property var lastTarget: null
    property alias text: field.text
    readonly property string message: messageLabel.text

    title: qsTr("Go to")
    modal: true
    anchors.centerIn: parent
    width: 420
    closePolicy: Popup.CloseOnEscape

    footer: DialogButtonBox {
        // Not standard Ok/Cancel buttons: Ok would close the dialog before the target is looked up.
        Button {
            text: qsTr("Go")
            objectName: "gotoGo"
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
            onClicked: if (dialog.submit()) dialog.close()
        }
        Button {
            text: qsTr("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: dialog.close()
        }
    }

    function submit() {
        const parsed = Goto.parse(field.text)
        if (!parsed.ok) {
            messageLabel.text = parsed.error
            return false
        }
        lastTarget = parsed
        const reason = typeof handler === "function" ? handler(parsed) : qsTr("no frame: no file is open")
        if (reason !== "" && reason !== undefined && reason !== null) {
            messageLabel.text = reason
            return false
        }
        messageLabel.text = ""
        return true
    }

    onOpened: {
        messageLabel.text = ""
        field.forceActiveFocus()
        field.selectAll()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        TextField {
            id: field

            objectName: "gotoField"
            Layout.fillWidth: true
            placeholderText: qsTr("d123, 123, poc45, 12.5s, 00:01:30.250, @0x1A2B3C, unit:12")
            Accessible.name: qsTr("Go to")
            onAccepted: if (dialog.submit()) dialog.close()
        }

        Label {
            id: messageLabel

            objectName: "gotoMessage"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: OvtStyle.dark ? "#ff8a80" : "#b71c1c"
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: OvtStyle.textMuted
            text: qsTr("d123 decode index · 123 or p123 display index · poc45 POC · 12.5s or 00:01:30.250 time · @0x1A2B3C or @1715004 byte offset · unit:N unit")
        }
    }
}
