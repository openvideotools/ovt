// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bookmarks & Comments panel (chapter 11 §11.3.17, WI-2.487): the Bookmarks list (colour dot, name, decode index)
// and the Comments list (anchor kind, excerpt, author, date) with Add bookmark (B), Add comment (Shift+B, inline
// editor), Export JSON / CSV and Import. A click navigates through Selection; rename is inline (double-click);
// right-click deletes or changes the colour. All data lives in the Bookmarks model and the project.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import OvtStudio

Item {
    id: panel

    objectName: "bookmarksPanel"
    // Set by the dock workspace when it creates the panel item (it passes {"panelId": id}).
    property string panelId: "bookmarks"
    implicitHeight: 260
    Accessible.name: "Bookmarks and comments"

    readonly property var colors: ["#e5484d", "#f5a623", "#46a758", "#3e9bff", "#a07cf0", "#e54d9e"]

    function openCommentEditor() {
        commentEditor.visible = true
        commentText.forceActiveFocus()
    }

    Connections {
        target: Bookmarks
        function onCommentRequested() {
            panel.openCommentEditor()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "addBookmarkButton"
                text: qsTr("Add bookmark")
                onClicked: Bookmarks.addBookmark()
            }
            Button {
                objectName: "addCommentButton"
                text: qsTr("Add comment")
                onClicked: panel.openCommentEditor()
            }
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Export JSON"); onClicked: pathPopup.openFor("json") }
            Button { text: qsTr("Export CSV"); onClicked: pathPopup.openFor("csv") }
            Button { text: qsTr("Import"); onClicked: pathPopup.openFor("import") }
        }

        RowLayout {
            id: commentEditor

            objectName: "commentEditor"
            visible: false
            Layout.fillWidth: true
            TextField {
                id: commentText

                objectName: "commentText"
                Layout.fillWidth: true
                placeholderText: qsTr("Comment on the current selection")
                Accessible.name: "Comment text"
                onAccepted: save.clicked()
                Keys.onEscapePressed: commentEditor.visible = false
            }
            Button {
                id: save

                text: qsTr("Save")
                onClicked: {
                    if (Bookmarks.addComment(commentText.text) !== "") {
                        commentText.clear()
                        commentEditor.visible = false
                    }
                }
            }
        }

        Label { text: qsTr("Bookmarks"); font.bold: true; color: OvtStyle.text }
        ListView {
            id: bookmarkList

            objectName: "bookmarkList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Bookmarks.bookmarks
            Accessible.name: "Bookmarks"

            delegate: ItemDelegate {
                id: row

                required property int index
                required property string id
                required property string name
                required property string color
                required property int decodeIndex

                width: bookmarkList.width
                height: 28
                objectName: "bookmarkRow"
                onClicked: Bookmarks.goToBookmark(id)
                onDoubleClicked: nameEdit.visible = true
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: menu.popup()
                }

                contentItem: RowLayout {
                    spacing: OvtStyle.spacing
                    Rectangle { width: 10; height: 10; radius: 5; color: row.color }
                    Label {
                        text: row.name
                        color: OvtStyle.text
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        visible: !nameEdit.visible
                    }
                    TextField {
                        id: nameEdit

                        visible: false
                        text: row.name
                        Layout.fillWidth: true
                        onAccepted: {
                            Bookmarks.renameBookmark(row.id, text)
                            visible = false
                        }
                        Keys.onEscapePressed: visible = false
                    }
                    Label { text: "#" + row.decodeIndex; color: OvtStyle.textMuted }
                }

                Menu {
                    id: menu

                    MenuItem { text: qsTr("Delete"); onTriggered: Bookmarks.removeBookmark(row.id) }
                    MenuItem { text: qsTr("Move up"); enabled: row.index > 0; onTriggered: Bookmarks.moveBookmark(row.index, row.index - 1) }
                    MenuItem {
                        text: qsTr("Move down")
                        enabled: row.index < bookmarkList.count - 1
                        onTriggered: Bookmarks.moveBookmark(row.index, row.index + 1)
                    }
                    Menu {
                        title: qsTr("Change colour")
                        Repeater {
                            model: panel.colors
                            MenuItem {
                                required property string modelData

                                text: modelData
                                onTriggered: Bookmarks.setBookmarkColor(row.id, modelData)
                            }
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: bookmarkList.count === 0
                text: qsTr("Press B to bookmark the current frame.")
                color: OvtStyle.textMuted
            }
        }

        Label { text: qsTr("Comments"); font.bold: true; color: OvtStyle.text }
        ListView {
            id: commentList

            objectName: "commentList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Bookmarks.comments
            Accessible.name: "Comments"

            delegate: ItemDelegate {
                id: crow

                required property string id
                required property string name
                required property string anchorKind
                required property string author
                required property string created

                width: commentList.width
                height: 28
                objectName: "commentRow"
                onClicked: Bookmarks.goToComment(id)
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: cmenu.popup()
                }

                contentItem: RowLayout {
                    spacing: OvtStyle.spacing
                    Label { text: crow.anchorKind; color: OvtStyle.textMuted; Layout.preferredWidth: 48 }
                    Label { text: crow.name; color: OvtStyle.text; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { text: crow.author; color: OvtStyle.textMuted }
                    Label { text: crow.created.substring(0, 10); color: OvtStyle.textMuted }
                }

                Menu {
                    id: cmenu

                    MenuItem { text: qsTr("Delete"); onTriggered: Bookmarks.removeComment(crow.id) }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: commentList.count === 0
                text: qsTr("Press Shift+B to comment on the current selection.")
                color: OvtStyle.textMuted
            }
        }
    }

    // Export and Import ask for a path in a field, as the Diagnostics panel's Save does: no QtQuick.Dialogs needed.
    Popup {
        id: pathPopup

        // "json", "csv" or "import".
        property string mode: "json"

        function openFor(kind) {
            mode = kind
            result.text = ""
            if (path.text === "") {
                const dir = StandardPaths.writableLocation(StandardPaths.DocumentsLocation).toString()
                path.text = decodeURIComponent(dir.replace(/^file:\/\//, "")) + "/ovt-bookmarks.json"
            }
            open()
        }

        objectName: "pathPopup"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(480, parent.width - 2 * OvtStyle.spacing)
        modal: true
        padding: OvtStyle.spacing

        ColumnLayout {
            anchors.fill: parent
            spacing: OvtStyle.spacing

            Label {
                text: pathPopup.mode === "import" ? qsTr("Import bookmarks and comments from") : qsTr("Export to")
                color: OvtStyle.text
            }
            TextField {
                id: path

                objectName: "pathField"
                Layout.fillWidth: true
                Accessible.name: "File path"
                onAccepted: go.clicked()
            }
            Label {
                id: result

                objectName: "pathResult"
                visible: text !== ""
                color: OvtStyle.textMuted
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Close"); onClicked: pathPopup.close() }
                Button {
                    id: go

                    text: pathPopup.mode === "import" ? qsTr("Import") : qsTr("Export")
                    onClicked: {
                        if (pathPopup.mode === "import") {
                            const n = Bookmarks.importJson(path.text)
                            result.text = n < 0 ? qsTr("Could not read the file.") : qsTr("%1 entries imported.").arg(n)
                        } else {
                            const ok = pathPopup.mode === "json" ? Bookmarks.exportJson(path.text) : Bookmarks.exportCsv(path.text)
                            result.text = ok ? qsTr("Saved.") : qsTr("Could not write the file.")
                        }
                    }
                }
            }
        }
    }
}
