// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Keyboard map dialog (chapter 11 §11.4.3, Help ▸ Keyboard map and Preferences ▸ Keyboard, FR-GUI-008): every
// action with its keys. "Change" waits for the next key press and rebinds the action to it; a key that another action of
// the same scope already uses is refused (GUI-KEY-0001) and the old binding stays. The map is saved to keymap.json.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Dialog {
    id: dialog

    objectName: "keymapDialog"
    // The action waiting for a key press, "" when none.
    property string capturing: ""
    property string error: ""

    title: qsTr("Keyboard map")
    modal: true
    anchors.centerIn: parent
    width: 640
    height: Math.min(560, parent ? parent.height - 80 : 560)
    standardButtons: Dialog.Close

    onClosed: capturing = ""
    onOpened: error = Keymap.diagnostics.join("\n")

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        Label {
            Layout.fillWidth: true
            visible: dialog.error !== ""
            text: dialog.error
            wrapMode: Text.Wrap
            color: OvtStyle.dark ? "#ff8a80" : "#b71c1c"
            objectName: "keymapError"
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.capturing !== ""
            text: qsTr("Press the new key for “%1” (Esc cancels)").arg(Keymap.text(dialog.capturing))
            font.bold: true
            objectName: "keymapPrompt"
        }

        ListView {
            id: list

            objectName: "keymapList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Keymap.actionIds
            ScrollBar.vertical: ScrollBar {}

            delegate: RowLayout {
                id: row

                required property string modelData
                width: ListView.view.width - 12
                spacing: OvtStyle.spacing

                Label {
                    Layout.fillWidth: true
                    text: Keymap.text(row.modelData)
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "keys_" + row.modelData
                    Layout.preferredWidth: 150
                    text: (Keymap.revision, Keymap.display(row.modelData))
                    font.family: "monospace"
                    color: OvtStyle.textMuted
                }
                Button {
                    text: qsTr("Change")
                    objectName: "change_" + row.modelData
                    onClicked: {
                        dialog.error = ""
                        dialog.capturing = row.modelData
                        capture.forceActiveFocus()
                    }
                }
                Button {
                    text: qsTr("Reset")
                    enabled: (Keymap.revision, Keymap.keys(row.modelData).join() !== Keymap.defaultKeys(row.modelData).join())
                    onClicked: Keymap.resetAction(row.modelData)
                }
            }
        }

        RowLayout {
            Button {
                text: qsTr("Reset all")
                objectName: "keymapResetAll"
                onClicked: Keymap.resetAll()
            }
            Label {
                color: OvtStyle.textMuted
                text: Keymap.path
            }
        }
    }

    // Takes the key press while a row waits for its new key.
    Item {
        id: capture

        objectName: "keymapCapture"
        focus: dialog.capturing !== ""
        Keys.onPressed: event => {
            if (dialog.capturing === "")
                return
            event.accepted = true
            if (event.key === Qt.Key_Escape) {
                dialog.capturing = ""
                return
            }
            const key = Keymap.keyText(event.key, event.modifiers)
            if (key === "")
                return  // a modifier alone: wait for the key
            const message = Keymap.setKeys(dialog.capturing, [key])
            dialog.error = message
            if (message === "")
                dialog.capturing = ""
        }
    }
}
