// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dialogs of WI-1.466 (Go to, Preferences, Keyboard map) and the actions that open them. Declared once in
// Main.qml; the shortcut layer is blocked while one is open.

import QtQuick
import QtQuick.Controls
import OvtStudio

QtObject {
    id: dialogs

    required property var actions
    required property Item overlayParent
    readonly property bool anyOpen: goToDialog.opened || preferencesDialog.opened || keymapDialog.opened

    property Connections connections: Connections {
        target: dialogs.actions

        function onActivated(id) {
            if (id === "edit.goTo")
                goToDialog.open()
            else if (id === "edit.preferences")
                preferencesDialog.open()
            else if (id === "help.keyboardMap")
                keymapDialog.open()
        }
    }

    property GotoDialog goTo: GotoDialog {
        id: goToDialog

        parent: dialogs.overlayParent
        handler: dialogs.actions.goToHandler
    }

    property PreferencesDialog preferences: PreferencesDialog {
        id: preferencesDialog

        parent: dialogs.overlayParent
        onKeymapRequested: keymapDialog.open()
    }

    property KeymapDialog keymap: KeymapDialog {
        id: keymapDialog

        parent: dialogs.overlayParent
    }
}
