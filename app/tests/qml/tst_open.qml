// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.454 acceptance test 4 and the Raw format dialog (acceptance test 3, QML part): a file passed on the command
// line opens (Main.initialFile); a second file asks "Open as"; the Raw format dialog shows GUI-RAW-0001.

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Open"
    when: windowShown

    Component {
        id: mainComponent

        Main {
            visible: true
            title: "OVT Studio"
            initialFile: Qt.resolvedUrl("tst_open.qml")
        }
    }

    Component {
        id: rawComponent

        RawFormatDialog {
            parent: Overlay.overlay
        }
    }

    function init() {
        Project.closeAll()
    }

    function findByName(item, objectName) {
        if (item.objectName === objectName)
            return item
        const kids = item.children
        for (let i = 0; i < kids.length; ++i) {
            const found = findByName(kids[i], objectName)
            if (found)
                return found
        }
        return null
    }

    // A file given on the command line opens; a second file (drop) shows the Open-as dialog.
    function test_initialFileAndSecondFile() {
        const window = createTemporaryObject(mainComponent, null)
        verify(window !== null)
        compare(Project.fileCount, 1)
        compare(Project.files[0].name, "tst_open.qml")
        compare(Project.files[0].role, "single")
        Project.openFile(Qt.resolvedUrl("tst_main.qml"))
        compare(Project.fileCount, 1)  // not added yet: the dialog decides
        const dialog = window.openAsDialogItem
        verify(dialog !== null)
        tryVerify(function () { return dialog.visible })
        findByName(dialog.contentItem.children[0], "openAsReference").clicked()
        compare(Project.fileCount, 2)
        compare(Project.files[0].role, "distorted")
        compare(Project.files[1].role, "reference")
        tryVerify(function () { return !dialog.visible })
    }

    function test_rawDialogDiagnostic() {
        const dialog = createTemporaryObject(rawComponent, testCase)
        verify(dialog !== null)
        dialog.path = Qt.resolvedUrl("tst_main.qml").toString().replace("file://", "")
        verify(dialog.format.diagnosticCode === "GUI-RAW-0001")
        dialog.open()
        tryVerify(function () { return dialog.visible })
        const label = findByName(dialog.contentItem.children[0], "rawDiagnostic")
        verify(label !== null)
        verify(label.visible)
        verify(label.text.indexOf("GUI-RAW-0001") === 0)
    }
}
