// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HDR panel (chapter 11 §11.3.14; WI-1.462 skeleton). Tabs:
//  - Metadata: item, value, provenance of every HDR/colour item the core merged (HdrMetadataModel), with a ⚠ row for
//    each core diagnostic (DEC-HDR-0001 when container and bitstream disagree).
//  - Measurements: the declared MaxCLL/MaxFALL; the per-frame measurement, the range scan and the declared-vs-measured
//    verdict (VQ-HDR-0002, computed by the core) are wired by a later work item, so they read "—" here.
//  - Display: the HDR output request, the ViewState HDR controls (HdrView), the macOS encoding and the Dolby label.
// Scopes arrive with the scope textures of phase 4 (WI-4.454). Nothing here computes or classifies codec data.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Item {
    id: panel

    objectName: "hdrPanel"
    // Set by the dock workspace when it creates the panel item (it passes {"panelId": id}).
    property string panelId: "hdr"
    implicitHeight: 320
    Accessible.name: "HDR"

    readonly property alias currentTab: tabs.currentIndex

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabs

            objectName: "hdrTabs"
            Layout.fillWidth: true
            TabButton {
                objectName: "metadataTab"
                text: HdrMetadataModel.warningCount > 0 ? qsTr("Metadata ⚠") : qsTr("Metadata")
            }
            TabButton {
                objectName: "measurementsTab"
                text: qsTr("Measurements")
            }
            TabButton {
                objectName: "displayTab"
                text: qsTr("Display")
            }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ---- Metadata ----
            ColumnLayout {
                spacing: OvtStyle.spacing

                Label {
                    objectName: "hdrKind"
                    Layout.margins: OvtStyle.spacing
                    text: HdrMetadataModel.hasMetadata ? HdrMetadataModel.kindText : qsTr("No HDR metadata (open a file).")
                    font.bold: HdrMetadataModel.hasMetadata
                    color: HdrMetadataModel.hasMetadata ? OvtStyle.text : OvtStyle.textMuted
                }
                ListView {
                    id: metadataList

                    objectName: "metadataList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: HdrMetadataModel
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        id: rowItem

                        required property int index
                        required property string item
                        required property string value
                        required property string provenance
                        required property bool warning
                        required property string code

                        objectName: "metadataRow" + index
                        width: ListView.view.width
                        height: Math.max(OvtStyle.minHitTarget, rowLayout.implicitHeight + 4)
                        color: index % 2 === 0 ? OvtStyle.panel : OvtStyle.window
                        Accessible.name: (warning ? "Warning " : "") + item + ": " + value

                        RowLayout {
                            id: rowLayout

                            anchors.fill: parent
                            anchors.leftMargin: OvtStyle.spacing
                            anchors.rightMargin: OvtStyle.spacing
                            spacing: OvtStyle.spacing
                            Label {
                                objectName: "warningIcon"
                                text: "⚠"
                                visible: rowItem.warning
                                color: OvtStyle.error
                                ToolTip.visible: hover.hovered && rowItem.code !== ""
                                ToolTip.text: rowItem.code
                                HoverHandler { id: hover }
                            }
                            Label {
                                text: rowItem.item
                                Layout.preferredWidth: 180
                                elide: Text.ElideRight
                                font.bold: rowItem.warning
                            }
                            Label {
                                text: rowItem.value
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }
                            Label {
                                text: rowItem.provenance
                                color: OvtStyle.textMuted
                                Layout.preferredWidth: 130
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            // ---- Measurements ----
            GridLayout {
                objectName: "measurementsGrid"
                columns: 3
                columnSpacing: OvtStyle.spacing * 2
                rowSpacing: OvtStyle.spacing

                Label { text: ""; font.bold: true }
                Label { text: qsTr("Declared"); font.bold: true }
                Label { text: qsTr("Measured"); font.bold: true; Layout.fillWidth: true }

                Label { text: qsTr("MaxCLL") }
                Label {
                    objectName: "declaredMaxCll"
                    text: HdrMetadataModel.declaredMaxCll > 0 ? HdrMetadataModel.declaredMaxCll + " cd/m²" : "—"
                }
                Label { text: "—"; color: OvtStyle.textMuted }

                Label { text: qsTr("MaxFALL") }
                Label {
                    objectName: "declaredMaxFall"
                    text: HdrMetadataModel.declaredMaxFall > 0 ? HdrMetadataModel.declaredMaxFall + " cd/m²" : "—"
                }
                Label { text: "—"; color: OvtStyle.textMuted }

                Label {
                    Layout.columnSpan: 3
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: OvtStyle.textMuted
                    text: HdrMetadataModel.isSdr ? qsTr("SDR content — measurements use 203 cd/m² reference white.")
                                                 : qsTr("Per-frame measurement and the range scan are not available yet.")
                }
                Button {
                    objectName: "scanRangeButton"
                    text: qsTr("Scan range…")
                    enabled: false
                }
                Item { Layout.fillHeight: true; Layout.columnSpan: 2 }
            }

            // ---- Display ----
            ScrollView {
                id: displayScroll

                clip: true
                contentWidth: availableWidth

                GridLayout {
                    width: displayScroll.availableWidth
                    columns: 2
                    columnSpacing: OvtStyle.spacing * 2
                    rowSpacing: OvtStyle.spacing

                    Label { text: qsTr("HDR output") }
                    // R-ARCH-2: NATIVE mode only on an HDR-capable display; otherwise the toggle says why not.
                    Item {
                        implicitWidth: hdrOutputToggle.implicitWidth
                        implicitHeight: hdrOutputToggle.implicitHeight
                        Switch {
                            id: hdrOutputToggle

                            objectName: "hdrOutputToggle"
                            readonly property string toolTipText: DisplayCaps.hdrCapable ? qsTr("Show HDR on this display (NATIVE mode)")
                                                                                         : DisplayCaps.reason
                            enabled: DisplayCaps.hdrCapable
                            checked: HdrView.hdrOutput && DisplayCaps.hdrCapable
                            onToggled: HdrView.hdrOutput = checked
                            Accessible.name: "HDR output"
                            Accessible.description: toolTipText
                        }
                        // A disabled control gets no hover, so the tooltip hangs on its parent.
                        HoverHandler { id: toggleHover }
                        ToolTip.visible: toggleHover.hovered
                        ToolTip.text: hdrOutputToggle.toolTipText
                    }

                    Label { text: qsTr("Tone map") }
                    ComboBox {
                        objectName: "toneMapCombo"
                        model: ["Auto", "BT.2390", "BT.2446a", "Spline", "Clip", "None"]
                        currentIndex: HdrView.toneMap
                        onActivated: HdrView.toneMap = currentIndex
                        Accessible.name: "Tone map"
                    }

                    Label { text: qsTr("Clip at (cd/m²)") }
                    SpinBox {
                        objectName: "clipNits"
                        from: 1
                        to: 10000
                        stepSize: 50
                        editable: true
                        enabled: HdrView.toneMap === 4
                        value: HdrView.clipNits
                        onValueModified: HdrView.clipNits = value
                        Accessible.name: "Clip nits"
                    }

                    Label { text: qsTr("Gamut map") }
                    ComboBox {
                        objectName: "gamutMapCombo"
                        model: ["Perceptual", "Relative", "Saturation", "Absolute", "Desaturate", "Darken", "Clip",
                                "Highlight"]
                        currentIndex: HdrView.gamutMap
                        onActivated: HdrView.gamutMap = currentIndex
                        Accessible.name: "Gamut map"
                    }

                    Label { text: qsTr("Target peak (cd/m², 0 = display)") }
                    SpinBox {
                        objectName: "targetPeak"
                        from: 0
                        to: 10000
                        stepSize: 100
                        editable: true
                        value: HdrView.targetPeakOverride
                        onValueModified: HdrView.targetPeakOverride = value
                        Accessible.name: "Target peak override"
                    }

                    Label { text: qsTr("Views") }
                    ColumnLayout {
                        spacing: 0
                        CheckBox {
                            objectName: "falseColorToggle"
                            text: qsTr("False colour (F12)")
                            checked: HdrView.falseColor
                            onToggled: HdrView.falseColor = checked
                        }
                        CheckBox {
                            objectName: "gamutClipToggle"
                            text: qsTr("Gamut-clip overlay")
                            checked: HdrView.gamutClipOverlay
                            onToggled: HdrView.gamutClipOverlay = checked
                        }
                        CheckBox {
                            objectName: "ditherToggle"
                            text: qsTr("Dither")
                            checked: HdrView.dither
                            onToggled: HdrView.dither = checked
                        }
                    }

                    Label {
                        text: qsTr("macOS encoding")
                        visible: Platform.isMacOS
                    }
                    ComboBox {
                        objectName: "macEncodingCombo"
                        visible: Platform.isMacOS
                        model: ["scRGB (extended linear)", "System PQ"]
                        currentIndex: HdrView.macEncoding
                        onActivated: HdrView.macEncoding = currentIndex
                        Accessible.name: "macOS encoding"
                    }

                    Label { text: qsTr("Dolby Vision") }
                    Label {
                        objectName: "dolbyLabel"
                        text: HdrMetadataModel.dolbyLabel !== "" ? HdrMetadataModel.dolbyLabel : "—"
                    }

                    Button {
                        text: qsTr("Reset")
                        onClicked: HdrView.reset()
                    }
                }
            }
        }
    }
}
