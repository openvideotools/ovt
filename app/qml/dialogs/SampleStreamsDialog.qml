// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The consent dialog of the Sample streams list (chapter 11 §11.8, WI-1.464): shows the size and the licence of the
// stream and asks before anything is downloaded. It is shown every time, for every manifest entry. `consented` is
// the only way a download starts; this dialog does not fetch anything.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Dialog {
    id: dialog

    // One entry of SampleStreams.entries.
    property var entry: ({})

    signal consented(string id)

    objectName: "sampleStreamsDialog"
    title: qsTr("Download sample stream")
    modal: true
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 2 * OvtStyle.spacing : 520, 520)

    onAccepted: consented(entry.id ?? "")

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("%1 will be downloaded from the address below. Nothing is downloaded unless you agree.").arg(dialog.entry.id ?? "")
        }
        Label {
            objectName: "sampleSize"
            Layout.fillWidth: true
            text: qsTr("Size: %1").arg(dialog.entry.sizeText ?? "")
        }
        Label {
            objectName: "sampleLicence"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Licence: %1 (%2)").arg(dialog.entry.licence ?? "").arg(dialog.entry.licenceSpdx ?? "")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            color: OvtStyle.textMuted
            text: dialog.entry.url ?? ""
        }
    }

    footer: DialogButtonBox {
        Button {
            objectName: "sampleDownload"
            text: qsTr("Download")
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            text: qsTr("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }
}
