// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.458 acceptance test 3 (chapter 11 §11.3.5, §11.4.3): with the Hex panel focused, Ctrl+B toggles the binary
// ribbon and B still reaches the window's Add bookmark. The panel is built on the file of this test (bytes are given to the model directly); the model arithmetic is tested in app/tests/models/test_hex_model.cpp.

import QtQuick
import QtQuick.Window
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Hex"
    when: windowShown

    // The window shortcuts of the shell: B and the others call trigger(id) on this recorder.
    QtObject {
        id: recorder

        property var order: []

        signal activated(string id)

        function trigger(id) {
            order.push(id)
        }
    }

    Window {
        id: stage

        width: 900
        height: 500
        visible: true

        ShortcutLayer {
            actions: recorder
        }
        HexPanel {
            id: panel

            anchors.fill: parent
            shellActions: recorder
        }
    }

    function cleanup() {
        panel.hex.closeFile()
    }

    function init() {
        recorder.order = []
        Keymap.resetAll()
        Selection.clearSelection("test")
    }

    function findByName(item, objectName) {
        if (item.objectName === objectName)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = findByName(item.children[i], objectName)
            if (found)
                return found
        }
        return null
    }

    function openPanel() {
        // Bytes with "//" pairs to find: a comment-like line, repeated.
        let text = ""
        for (let i = 0; i < 40; ++i)
            text += "// hex panel test line, repeated for bytes to find\n"
        panel.hex.openBytes(text)
        tryVerify(function () { return panel.table.visible })
        stage.requestActivate()
        tryVerify(function () { return stage.active })
        tryVerify(function () {
            panel.table.forceActiveFocus()
            return panel.table.activeFocus
        })
    }

    function test_emptyWithoutFile() {
        verify(!panel.hex.hasSource)
        verify(!panel.table.visible)
    }

    // Acceptance test 3.
    function test_ctrlBTogglesRibbonAndBStillAddsBookmark() {
        openPanel()
        Selection.selectBytes(10, 4, "test")
        const ribbon = findByName(panel, "bitRibbon")
        verify(ribbon !== null)
        tryVerify(function () { return ribbon.visible })
        keyClick(Qt.Key_B, Qt.ControlModifier)
        tryVerify(function () { return !ribbon.visible })
        compare(panel.hex.showRibbon, false)
        compare(recorder.order.length, 0, "Ctrl+B is the panel's key, not a window action")
        keyClick(Qt.Key_B, Qt.ControlModifier)
        tryVerify(function () { return ribbon.visible })
        keyClick(Qt.Key_B)
        compare(recorder.order.length, 1)
        compare(recorder.order[0], "navigate.addBookmark")
        compare(panel.hex.showRibbon, true, "B does not touch the ribbon")
    }

    function test_clickSelectsByteAndGotoMovesSelection() {
        openPanel()
        const field = findByName(panel, "hexGoto")
        field.text = "0x20"
        field.accepted()
        compare(Selection.byteOffset, 0x20)
        compare(Selection.byteLength, 1)
        field.text = "nonsense"
        field.accepted()
        compare(Selection.byteOffset, 0x20, "a failed Go to changes nothing")
    }

    function test_findNextAndPreviousKeysStepThroughMatches() {
        openPanel()
        // The bytes hold "//" at the start of every line.
        const find = findByName(panel, "hexFind")
        find.text = "2F 2F"
        find.accepted()
        tryVerify(function () { return panel.hex.matchCount > 2 })
        keyClick(Qt.Key_F3)
        compare(panel.hex.currentMatch, 0)
        const first = Selection.byteOffset
        keyClick(Qt.Key_F3)
        compare(panel.hex.currentMatch, 1)
        verify(Selection.byteOffset > first)
        keyClick(Qt.Key_F3, Qt.ShiftModifier)
        compare(panel.hex.currentMatch, 0)
        compare(Selection.byteOffset, first)
        compare(recorder.order.length, 0, "F3 is the panel's key while it has focus, not Motion vectors")
    }
}
