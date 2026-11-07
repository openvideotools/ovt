// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.466 acceptance test 3: every window-level row of the §11.4.3 table triggers its action exactly once when its key
// is sent, a rebinding in the Keymap moves the shortcut, and the Go to dialog parses and reports "no frame".

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Shortcuts"
    when: windowShown
    width: 400
    height: 300

    // Records every trigger(id) the shortcut layer makes.
    QtObject {
        id: recorder

        property var counts: ({})
        property var order: []

        function trigger(id) {
            counts[id] = (counts[id] || 0) + 1
            order.push(id)
        }
        function reset() {
            counts = ({})
            order = []
        }
    }

    ShortcutLayer {
        id: layer

        actions: recorder
    }

    Component {
        id: dialogComponent

        GotoDialog {
            parent: testCase
        }
    }

    function init() {
        recorder.reset()
        layer.blocked = false
        Keymap.resetAll()
    }

    function menuId(actionId) {
        return actionId.substring("action.".length)
    }

    function press(keyText) {
        const parts = Keymap.decompose(keyText)
        verify(parts.length === 2, "cannot send " + keyText)
        keyClick(parts[0], parts[1])
    }

    // Every window-scope row of the table, one key press each: the action fires once and nothing else fires.
    function test_everyRowFiresOnce() {
        const ids = Keymap.windowActionIds
        verify(ids.length > 80)
        for (let i = 0; i < ids.length; ++i) {
            recorder.reset()
            const keys = Keymap.defaultKeys(ids[i])
            for (let k = 0; k < keys.length; ++k) {
                press(keys[k])
                compare(recorder.order.length, k + 1, ids[i] + " with " + keys[k])
                compare(recorder.order[k], menuId(ids[i]), keys[k])
            }
        }
    }

    function test_rebindMovesTheShortcut() {
        compare(Keymap.setKeys("action.navigate.next", ["Ctrl+J"]), "GUI-KEY-0001: shortcut Ctrl+J is bound to action.view.jobs and action.navigate.next")
        compare(Keymap.setKeys("action.view.jobs", ["Ctrl+K"]), "")
        compare(Keymap.setKeys("action.navigate.next", ["Ctrl+J"]), "")
        press("Right")
        compare(recorder.order.length, 0)
        press("Ctrl+J")
        compare(recorder.order, ["navigate.next"])
        recorder.reset()
        press("Ctrl+K")
        compare(recorder.order, ["view.jobs"])
    }

    function test_blockedWhileADialogIsOpen() {
        layer.blocked = true
        press("Space")
        compare(recorder.order.length, 0)
    }

    function test_gotoDialog() {
        const dialog = createTemporaryObject(dialogComponent, testCase)
        verify(dialog !== null)
        // Nothing handles the request (no file open): the answer is "no frame" and the dialog stays open.
        dialog.open()
        dialog.text = "poc9999"
        verify(!dialog.submit())
        verify(dialog.message.indexOf("no frame") === 0)
        compare(dialog.lastTarget.kind, "poc")
        compare(dialog.lastTarget.value, 9999)
        // Text outside the grammar is refused with a reason before any lookup.
        dialog.text = "nonsense"
        verify(!dialog.submit())
        verify(dialog.message !== "")
        // A handler that navigates closes the request with an empty reason, and gets the parsed target.
        let seen = null
        dialog.handler = function (target) { seen = target; return "" }
        dialog.text = "@0x1A3F40"
        verify(dialog.submit())
        compare(seen.kind, "byte")
        compare(seen.value, 0x1A3F40)
        compare(dialog.message, "")
        // A handler that refuses puts its reason on screen.
        dialog.handler = function (target) { return "no frame with POC " + target.value }
        dialog.text = "poc9999"
        verify(!dialog.submit())
        compare(dialog.message, "no frame with POC 9999")
    }
}
