// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window-level shortcuts of chapter 11 §11.4.3 (WI-1.466): one Shortcut per window-scope action of the Keymap,
// bound to the keys the user has now (keymap.json). A key press calls `actions.trigger(id)` with the menu id (the
// action id without "action."), exactly once; the Actions object decides whether the action does anything yet.
// Single-letter shortcuts never fire while a text field has focus (the field takes the key first, Qt's
// ShortcutOverride) and nothing fires while a modal dialog is open (`blocked`).

import QtQuick
import OvtStudio

Item {
    id: layer

    // Anything with a trigger(id) function: the shell's Actions, or a recorder in tests.
    required property var actions
    property bool blocked: false

    visible: false

    Instantiator {
        model: Keymap.windowActionIds

        delegate: Shortcut {
            required property string modelData

            sequences: (Keymap.revision, Keymap.keys(modelData))
            enabled: !layer.blocked
            context: Qt.WindowShortcut
            onActivated: layer.actions.trigger(modelData.substring("action.".length))
        }
    }
}
