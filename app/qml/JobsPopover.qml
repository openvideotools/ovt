// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Jobs popover (chapter 11 §11.3.18, WI-1.453): running, queued and finished jobs with name, progress and ETA,
// a Cancel button per job and "Cancel all". Finished jobs keep their errors for 5 minutes (JobsModel). Opened from the
// toolbar indicator and by Ctrl+J.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Popup {
    id: popup

    objectName: "jobsPopover"
    width: 480
    height: Math.min(360, header.implicitHeight + Math.max(1, list.contentHeight) + 2 * padding + OvtStyle.spacing)
    padding: OvtStyle.spacing
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    background: Rectangle {
        color: OvtStyle.panel
        border.color: OvtStyle.border
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        RowLayout {
            id: header

            Layout.fillWidth: true
            Label {
                text: JobsModel.activeCount > 0 ? "Jobs · " + JobsModel.activeCount + " active" : "Jobs"
                font.bold: true
                color: OvtStyle.text
                Layout.fillWidth: true
            }
            Button {
                objectName: "cancelAll"
                text: "Cancel all"
                enabled: JobsModel.activeCount > 0
                onClicked: JobsModel.cancelAll()
            }
        }

        ListView {
            id: list

            objectName: "jobsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: JobsModel
            spacing: OvtStyle.spacing

            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "No jobs"
                color: OvtStyle.textMuted
            }

            delegate: ColumnLayout {
                id: row

                required property var jobId
                required property string name
                required property string state
                required property double progress
                required property string text
                required property string error
                required property bool finished

                width: ListView.view.width
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: row.text
                        elide: Text.ElideRight
                        color: row.state === "failed" ? OvtStyle.error : OvtStyle.text
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "Cancel"
                        visible: !row.finished
                        onClicked: JobsModel.cancel(row.jobId)
                        Accessible.name: "Cancel " + row.name
                    }
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: !row.finished
                    from: 0
                    to: 1
                    value: row.progress
                    indeterminate: row.progress < 0
                }
            }
        }
    }
}
