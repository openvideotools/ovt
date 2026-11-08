// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Welcome page (chapter 11 §11.3.19, §11.8; WI-1.464): shown in the Center area while no file is open. Open file
// and Open project, the drop zone, the recent files with pin and remove, the sample streams (behind a consent dialog
// that shows size and licence) and the three-line "How it works" with the keyboard map link.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Rectangle {
    id: page

    objectName: "welcomePage"

    // The shell's Actions: the Open buttons trigger file.open / file.openProject once they are enabled.
    property var actions: null
    // The sample streams the consent dialog is for (tests replace it).
    property var sampleEntries: SampleStreams.entries

    signal sampleConsented(string id)

    color: OvtStyle.panel
    Accessible.name: qsTr("Welcome")

    function actionEnabled(id) {
        return actions !== null && actions.isEnabled(id)
    }

    ScrollView {
        id: scroll

        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: Math.min(scroll.availableWidth - 2 * OvtStyle.spacing * 4, 720)
            x: Math.max(OvtStyle.spacing * 4, (scroll.availableWidth - width) / 2)
            y: OvtStyle.spacing * 4
            spacing: OvtStyle.spacing * 2

            Label {
                text: Qt.application.displayName
                font.pixelSize: OvtStyle.fontSize * 2
                font.bold: true
                color: OvtStyle.text
                Accessible.role: Accessible.Heading
            }

            RowLayout {
                spacing: OvtStyle.spacing * 2

                Button {
                    objectName: "welcomeOpenFile"
                    text: qsTr("Open file… (%1)").arg(page.actions ? page.actions.shortcutText("file.open") : "Ctrl+O")
                    highlighted: true
                    enabled: page.actionEnabled("file.open")
                    onClicked: page.actions.trigger("file.open")
                }
                Button {
                    objectName: "welcomeOpenProject"
                    text: qsTr("Open project…")
                    enabled: page.actionEnabled("file.openProject")
                    onClicked: page.actions.trigger("file.openProject")
                }
            }

            Rectangle {
                objectName: "welcomeDropZone"
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                color: "transparent"
                border.color: OvtStyle.border
                border.width: 2
                radius: OvtStyle.radius
                Accessible.name: dropLabel.text

                Label {
                    id: dropLabel

                    anchors.centerIn: parent
                    text: qsTr("Drop a video, elementary stream or .ovtproj")
                    color: OvtStyle.textMuted
                }
            }

            Label {
                text: qsTr("Recent files")
                font.bold: true
                color: OvtStyle.text
                Accessible.role: Accessible.Heading
            }
            EmptyState {
                objectName: "welcomeNoRecent"
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                visible: RecentFiles.entries.length === 0
                text: qsTr("No recent files.")
            }
            Repeater {
                objectName: "welcomeRecent"
                model: RecentFiles.entries
                delegate: RowLayout {
                    id: row

                    required property var modelData
                    required property int index

                    objectName: "recentRow_" + index
                    Layout.fillWidth: true
                    spacing: OvtStyle.spacing

                    ItemDelegate {
                        objectName: "recentOpen_" + row.index
                        Layout.fillWidth: true
                        onClicked: Project.openFile(row.modelData.path)
                        Accessible.name: row.modelData.name
                        contentItem: ColumnLayout {
                            spacing: 0

                            Label {
                                text: row.modelData.name
                                color: OvtStyle.text
                                font.bold: row.modelData.pinned
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Label {
                                text: row.modelData.path + (row.modelData.codec !== "" ? "  ·  " + row.modelData.codec : "")
                                      + "  ·  " + Qt.formatDateTime(row.modelData.lastOpened, Qt.DefaultLocaleShortDate)
                                color: OvtStyle.textMuted
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                    }
                    Button {
                        objectName: "recentPin_" + row.index
                        text: row.modelData.pinned ? qsTr("Unpin") : qsTr("Pin")
                        onClicked: RecentFiles.setPinned(row.modelData.path, !row.modelData.pinned)
                    }
                    Button {
                        objectName: "recentRemove_" + row.index
                        text: qsTr("Remove")
                        onClicked: RecentFiles.remove(row.modelData.path)
                    }
                }
            }

            Label {
                text: qsTr("Sample streams")
                font.bold: true
                color: OvtStyle.text
                visible: page.sampleEntries.length > 0
                Accessible.role: Accessible.Heading
            }
            Repeater {
                model: page.sampleEntries
                delegate: Button {
                    required property var modelData
                    required property int index

                    objectName: "sample_" + index
                    flat: true
                    text: modelData.id + "  (" + modelData.sizeText + ")"
                    onClicked: {
                        consent.entry = modelData
                        consent.open()
                    }
                }
            }

            Label {
                text: qsTr("How it works")
                font.bold: true
                color: OvtStyle.text
                Accessible.role: Accessible.Heading
            }
            Label {
                objectName: "welcomeHowItWorks"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: OvtStyle.text
                text: qsTr("1. The Timeline shows every frame; click one to seek.\n2. The Picture shows it, with overlays for the analysis.\n3. The Syntax and Hex panels show where it comes from; select in either and the other follows.")
            }
            Button {
                objectName: "welcomeKeyboardMap"
                flat: true
                text: qsTr("Keyboard map")
                enabled: page.actionEnabled("help.keyboardMap")
                onClicked: page.actions.trigger("help.keyboardMap")
            }
        }
    }

    SampleStreamsDialog {
        id: consent

        parent: Overlay.overlay
        onConsented: id => page.sampleConsented(id)
    }
}
