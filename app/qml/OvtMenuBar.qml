// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The menu bar of the main window: one OvtMenu per entry of Actions.menus (chapter 11 §11.2.3, 8 menus).

import QtQuick
import QtQuick.Controls

MenuBar {
    id: bar

    required property var actions

    Component {
        id: menuComponent

        OvtMenu {}
    }

    Component.onCompleted: {
        for (const entry of actions.menus) {
            const menu = menuComponent.createObject(bar.contentItem, {"title": entry.text, "entries": entry.items, "actions": bar.actions, "menuComponent": menuComponent})
            menu.populate()
            bar.addMenu(menu)
        }
    }
}
