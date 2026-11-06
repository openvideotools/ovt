// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A menu built from entries of Actions.menus (WI-1.450). Submenus are OvtMenu again (made from `menuComponent`).

import QtQuick
import QtQuick.Controls

Menu {
    id: menu

    required property var actions
    // The component of a menu, handed down from OvtMenuBar: a menu cannot instantiate its own type (QML rejects
    // the recursion), and the Component needs no recursion when it comes from outside.
    required property Component menuComponent
    property var entries: []

    Component {
        id: itemComponent

        MenuItem {
            id: menuItem

            required property var entry

            action: Action {
                text: menuItem.entry.text
                shortcut: menuItem.entry.shortcut
                checkable: menuItem.entry.checkable
                enabled: menu.actions.isEnabled(menuItem.entry.id)
                checked: menu.actions.isChecked(menuItem.entry.id)
                onTriggered: menu.actions.trigger(menuItem.entry.id)
            }
        }
    }

    Component {
        id: separatorComponent

        MenuSeparator {}
    }

    // Called by the creator once createObject() has returned: building the submenus from the component that is
    // still being completed would fail ("Cannot create new component instance before completing the previous").
    function populate() {
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            if (entry.separator === true) {
                menu.addItem(separatorComponent.createObject(menu.contentItem))
            } else if (entry.items !== undefined) {
                const sub = menuComponent.createObject(menu.contentItem, {"title": entry.text, "entries": entry.items, "actions": menu.actions, "menuComponent": menu.menuComponent})
                sub.populate()
                menu.addMenu(sub)
            } else {
                menu.addItem(itemComponent.createObject(menu.contentItem, {"entry": entry}))
            }
        }
    }
}
