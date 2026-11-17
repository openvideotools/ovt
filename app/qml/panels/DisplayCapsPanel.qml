// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Display Capabilities panel (chapter 11 §11.3.15; WI-1.462): the key/value table of chapter 10 §10.4.5 "Panel
// contents" for the screen hosting the Picture (DisplayCaps, from display_probe()), with Re-probe (spinner while it
// runs), Show test pattern (toggle) and Copy report. Unknown values read "—" with the reason.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Item {
    id: panel

    objectName: "displayCapsPanel"
    // Set by the dock workspace when it creates the panel item (it passes {"panelId": id}).
    property string panelId: "display_caps"
    implicitHeight: 280
    Accessible.name: "Display capabilities"

    // The screen of the window this panel lives in (the Picture's window in the docked layout).
    readonly property string screenName: Screen.name
    Binding {
        target: DisplayCaps
        property: "screenName"
        value: panel.screenName
        when: panel.screenName !== ""
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: OvtStyle.spacing
            Button {
                objectName: "reprobeButton"
                text: qsTr("Re-probe")
                enabled: !DisplayCaps.probing
                onClicked: DisplayCaps.reprobe()
            }
            BusyIndicator {
                objectName: "probeSpinner"
                running: DisplayCaps.probing
                visible: running
                Layout.preferredHeight: 24
                Layout.preferredWidth: 24
            }
            Button {
                objectName: "testPatternButton"
                text: qsTr("Show test pattern")
                checkable: true
                checked: DisplayCaps.testPattern
                onToggled: DisplayCaps.testPattern = checked
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "copyReportButton"
                text: qsTr("Copy report")
                onClicked: DisplayCaps.copyReport()
            }
        }

        ListView {
            objectName: "capsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: DisplayCaps
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: capsRow

                required property int index
                required property string key
                required property string value
                required property bool unknown

                width: ListView.view.width
                height: Math.max(OvtStyle.minHitTarget, valueLabel.implicitHeight + 4)
                color: index % 2 === 0 ? OvtStyle.panel : OvtStyle.window
                Accessible.name: key + ": " + value

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: OvtStyle.spacing
                    anchors.rightMargin: OvtStyle.spacing
                    spacing: OvtStyle.spacing
                    Label {
                        text: capsRow.key
                        Layout.preferredWidth: 220
                        elide: Text.ElideRight
                    }
                    Label {
                        id: valueLabel

                        text: capsRow.value
                        color: capsRow.unknown ? OvtStyle.textMuted : OvtStyle.text
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
