// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "Open as" dialog of chapter 11 §11.8 (WI-1.454): shown when a second file is opened while one is open.
// [Replace] [Reference (B)] [Distorted (A), current becomes reference] [New window].

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Dialog {
    id: dialog

    // The file that was opened or dropped.
    property string path: ""
    // Emitted for "New window": the caller starts another instance with `path`.
    signal newWindowRequested(string path)

    objectName: "openAsDialog"
    title: "Open as"
    modal: true
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape

    function choose(choice) {
        Project.openAs(path, choice)
        close()
    }

    ColumnLayout {
        spacing: OvtStyle.spacing

        Label {
            Layout.preferredWidth: 380
            text: dialog.path
            width: 380
            elide: Text.ElideMiddle
            color: OvtStyle.textMuted
            Accessible.name: "File to open"
        }
        Button {
            objectName: "openAsReplace"
            text: "Replace"
            Layout.fillWidth: true
            onClicked: dialog.choose(Project.Replace)
        }
        Button {
            objectName: "openAsReference"
            text: "Reference (B)"
            Layout.fillWidth: true
            onClicked: dialog.choose(Project.Reference)
        }
        Button {
            objectName: "openAsDistorted"
            text: "Distorted (A), current becomes reference"
            Layout.fillWidth: true
            onClicked: dialog.choose(Project.Distorted)
        }
        Button {
            objectName: "openAsNewWindow"
            text: "New window"
            Layout.fillWidth: true
            onClicked: {
                dialog.newWindowRequested(dialog.path)
                dialog.close()
            }
        }
        Button {
            objectName: "openAsCancel"
            text: "Cancel"
            Layout.fillWidth: true
            onClicked: dialog.close()
        }
    }
}
