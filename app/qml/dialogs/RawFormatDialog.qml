// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Raw format dialog (chapter 11 §11.8, WI-1.454): width, height, chroma format, bit depth and frame rate of a
// raw .yuv file, a live luma preview of frame 0 and GUI-RAW-0001 when the size does not fit.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Dialog {
    id: dialog

    property alias path: format.path
    readonly property alias format: format

    objectName: "rawFormatDialog"
    title: "Raw format"
    modal: true
    anchors.centerIn: parent
    standardButtons: Dialog.Ok | Dialog.Cancel

    onAccepted: Project.addRaw(format.path, format.width, format.height, format.chromaFormat, format.bitDepth,
                               format.fps)

    RawFormatModel {
        id: format
    }

    RowLayout {
        spacing: OvtStyle.spacing * 2

        GridLayout {
            columns: 2
            Label { text: "Width" }
            SpinBox {
                objectName: "rawWidth"
                from: 1; to: 16384; editable: true
                value: format.width
                onValueModified: format.width = value
                Accessible.name: "Width"
            }
            Label { text: "Height" }
            SpinBox {
                objectName: "rawHeight"
                from: 1; to: 16384; editable: true
                value: format.height
                onValueModified: format.height = value
                Accessible.name: "Height"
            }
            Label { text: "Chroma format" }
            ComboBox {
                objectName: "rawChroma"
                model: ["400", "420", "422", "444"]
                currentIndex: Math.max(0, model.indexOf(format.chromaFormat))
                onActivated: format.chromaFormat = currentText
                Accessible.name: "Chroma format"
            }
            Label { text: "Bit depth" }
            SpinBox {
                objectName: "rawBitDepth"
                from: 8; to: 16; editable: true
                value: format.bitDepth
                onValueModified: format.bitDepth = value
                Accessible.name: "Bit depth"
            }
            Label { text: "Frame rate" }
            SpinBox {
                objectName: "rawFps"
                from: 1; to: 240; editable: true
                value: Math.round(format.fps)
                onValueModified: format.fps = value
                Accessible.name: "Frame rate"
            }
            Label {
                objectName: "rawDiagnostic"
                Layout.columnSpan: 2
                visible: format.diagnosticCode !== ""
                text: format.diagnosticCode + ": " + format.diagnosticMessage
                color: OvtStyle.error
                wrapMode: Text.Wrap
                Layout.preferredWidth: 260
            }
            Label {
                Layout.columnSpan: 2
                visible: format.diagnosticCode === ""
                text: format.frameCount + " frames"
                color: OvtStyle.textMuted
            }
        }

        Image {
            objectName: "rawPreview"
            Layout.preferredWidth: 320
            Layout.preferredHeight: 180
            fillMode: Image.PreserveAspectFit
            cache: false
            source: format.previewSource
            Accessible.name: "Preview of frame 0"
        }
    }
}
