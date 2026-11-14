// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Diagnostics / Log panel (chapter 11 §11.3.16, WI-1.453): the Errors tab (severity, code, message, location, spec
// ref; filter by severity, module and text; double-click navigates) and the Log tab (level, module and text filters,
// pause/follow, Save…). The tab badge counts errors. Both tabs show the models' filtered views; nothing is parsed here.

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Item {
    id: panel

    objectName: "diagnosticsPanel"
    // Set by the dock workspace when it creates the panel item (it passes {"panelId": id}).
    property string panelId: "diagnostics"
    implicitHeight: 220
    Accessible.name: "Diagnostics"

    readonly property var diagnostics: DiagnosticsModel.filtered
    readonly property var log: LogSink.filtered

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabs

            Layout.fillWidth: true
            TabButton {
                objectName: "errorsTab"
                text: "Errors" + (DiagnosticsModel.errorCount > 0 ? " (" + DiagnosticsModel.errorCount + ")" : "")
                // The badge turns red when Errors > 0.
                contentItem: Label {
                    text: parent.text
                    horizontalAlignment: Text.AlignHCenter
                    color: DiagnosticsModel.errorCount > 0 ? "#ff6b6b" : OvtStyle.text
                    font.bold: DiagnosticsModel.errorCount > 0
                }
            }
            TabButton {
                objectName: "logTab"
                text: "Log"
            }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ---- Errors ----
            ColumnLayout {
                spacing: OvtStyle.spacing
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        objectName: "severityFilter"
                        model: ["All", "Warnings and errors", "Errors"]
                        onActivated: panel.diagnostics.minSeverity = currentIndex === 0 ? 0 : currentIndex
                        Accessible.name: "Severity filter"
                    }
                    TextField {
                        objectName: "diagnosticsText"
                        placeholderText: "Filter"
                        Layout.fillWidth: true
                        onTextChanged: panel.diagnostics.text = text
                        Accessible.name: "Diagnostics text filter"
                    }
                    Button {
                        text: "Copy all"
                        onClicked: copyBuffer.copy(panel.diagnostics.allText())
                    }
                }
                TextEdit {
                    id: copyBuffer

                    function copy(value) {
                        text = value
                        selectAll()
                        copy()
                    }
                    visible: false
                }
                Label {
                    visible: panel.diagnostics.rowCount === 0
                    text: "No diagnostics"
                    color: OvtStyle.textMuted
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    visible: DiagnosticsModel.droppedCount > 0
                    text: DiagnosticsModel.droppedCount + " more…"
                    color: OvtStyle.textMuted
                }
                ListView {
                    id: errors

                    objectName: "errorsList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: panel.diagnostics
                    delegate: ItemDelegate {
                        id: errorRow

                        required property int index
                        required property int severity
                        required property string code
                        required property string message
                        required property string location
                        required property string specRef

                        width: ListView.view.width
                        height: 24
                        text: [["Info", "Warning", "Error", "Fatal"][severity], code, message, location, specRef]
                                  .filter(s => s.length > 0).join(" · ")
                        onDoubleClicked: panel.diagnostics.activate(index)
                        contentItem: Label {
                            text: errorRow.text
                            elide: Text.ElideRight
                            color: errorRow.severity >= 2 ? "#ff6b6b" : (errorRow.severity === 1 ? "#f2c037" : OvtStyle.text)
                        }
                    }
                }
            }

            // ---- Log ----
            ColumnLayout {
                spacing: OvtStyle.spacing
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        objectName: "logLevel"
                        model: ["Trace", "Debug", "Info", "Warn", "Error"]
                        currentIndex: 3
                        onActivated: panel.log.minLevel = currentIndex
                        Component.onCompleted: panel.log.minLevel = currentIndex
                        Accessible.name: "Log level"
                    }
                    TextField {
                        objectName: "logText"
                        placeholderText: "Search"
                        Layout.fillWidth: true
                        onTextChanged: panel.log.text = text
                        Accessible.name: "Log search"
                    }
                    CheckBox {
                        id: follow

                        text: "Follow"
                        checked: true
                    }
                    Button {
                        objectName: "pauseLog"
                        text: LogSink.paused ? "Resume" : "Pause"
                        onClicked: LogSink.paused = !LogSink.paused
                    }
                    Button {
                        objectName: "saveLog"
                        text: "Save…"
                        onClicked: saveLogPopup.open()
                    }
                }
                ListView {
                    id: logView

                    objectName: "logList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: panel.log
                    onCountChanged: if (follow.checked) positionViewAtEnd()
                    delegate: Label {
                        required text

                        width: ListView.view.width
                        font.family: "monospace"
                        elide: Text.ElideRight
                        color: OvtStyle.text
                    }
                }
            }
        }
    }

    // "Save…" writes the lines the Log filter shows (LogSink::save, format of ovt::log::format_line). A path field
    // instead of a native file dialog keeps the panel free of QtQuick.Dialogs and testable offscreen.
    Popup {
        id: saveLogPopup

        objectName: "saveLogPopup"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(480, parent.width - 2 * OvtStyle.spacing)
        modal: true
        padding: OvtStyle.spacing
        onAboutToShow: {
            if (savePath.text === "") {
                const dir = StandardPaths.writableLocation(StandardPaths.DocumentsLocation).toString()
                savePath.text = decodeURIComponent(dir.replace(/^file:\/\//, "")) + "/ovt-log.txt"
            }
            saveResult.text = ""
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: OvtStyle.spacing

            Label {
                text: "Save the log lines shown to"
                color: OvtStyle.text
            }
            TextField {
                id: savePath

                objectName: "savePath"
                Layout.fillWidth: true
                Accessible.name: "Log file path"
                onAccepted: saveConfirm.clicked()
            }
            Label {
                id: saveResult

                objectName: "saveResult"
                visible: text !== ""
                color: OvtStyle.textMuted
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    text: "Close"
                    onClicked: saveLogPopup.close()
                }
                Button {
                    id: saveConfirm

                    objectName: "saveConfirm"
                    text: "Save"
                    enabled: savePath.text !== ""
                    onClicked: saveResult.text = LogSink.save(savePath.text) ? "Saved " + savePath.text : "Could not write " + savePath.text
                }
            }
        }
    }
}
