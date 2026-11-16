// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-A.450: the Android root item (AndroidShell.qml, which MainActivity's QtQuickView loads) hosts the desktop shell.
// Desktop half of acceptance test 1: on a fresh profile the Item root shows the Welcome page, the 8 menus, the toolbar
// and the status bar, as the desktop window does (tst_main.qml, tst_welcome.qml). The emulator half (the APK shows the
// Welcome page) is the manual record docs/acceptance/wi/WI-A.450.md.

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "AndroidShell"
    when: windowShown

    // QtQuickView's QQuickView plays this window's part on Android. The shell is created inside it, not under the
    // TestCase and reparented, so that its popups use this window's overlay from the start.
    Window {
        id: host

        width: 1280
        height: 800
        visible: true
    }

    Component {
        id: shellComponent

        AndroidShell {
            width: 1280
            height: 800
        }
    }

    function init() {
        Project.closeAll()
    }

    function visibleOnScreen(item) {
        return item !== null && item.visible && item.width > 0 && item.height > 0
    }

    // Every dock placeholder carries a WelcomePage that only the Picture panel shows, so findChild's first match may be
    // a hidden one: walk the visual tree for one that is on screen.
    function findShown(item, objectName) {
        if (item === null)
            return null
        if (item.objectName === objectName && visibleOnScreen(item))
            return item
        for (const kid of Array.from(item.children ?? [])) {
            const found = findShown(kid, objectName)
            if (found !== null)
                return found
        }
        return null
    }

    function createShell() {
        const shell = createTemporaryObject(shellComponent, host.contentItem)
        verify(shell !== null)
        // AndroidShell is an Item, not a Window: QtQuickView accepts nothing else.
        verify(shell instanceof Item)
        return shell
    }

    function test_welcomePageShows() {
        const shell = createShell()
        tryVerify(function () { return findShown(shell, "welcomePage") !== null })
        compare(findChild(shell, "statusFile").text, "No file open")
    }

    function test_menusToolBarAndStatusBar() {
        const shell = createShell()
        const menuBar = findChild(shell, "menuBar")
        verify(menuBar !== null)
        const titles = ["File", "Edit", "View", "Overlays", "Navigate", "Analysis", "Compare", "Help"]
        compare(menuBar.count, 8)
        for (let i = 0; i < titles.length; ++i)
            compare(menuBar.menuAt(i).title, titles[i])
        tryVerify(function () { return visibleOnScreen(findChild(shell, "toolBar")) })
        compare(shell.footer.objectName, "statusBar")
        verify(visibleOnScreen(shell.footer))
    }

    // The toolbar's Jobs indicator opens the shell's Jobs popover below the toolbar, as on the desktop.
    function test_jobsPopover() {
        const shell = createShell()
        const popover = shell.shellItem.jobsPopoverItem
        verify(!popover.opened)
        findChild(shell, "jobsButton").clicked()
        tryVerify(function () { return popover.opened })
        verify(popover.parent.Window.window === host)
        const toolBarBottom = shell.header.mapToItem(popover.parent, 0, shell.header.height).y
        verify(popover.y >= toolBarBottom, "popover y " + popover.y + " above the toolbar bottom " + toolBarBottom)
        popover.close()
    }
}
