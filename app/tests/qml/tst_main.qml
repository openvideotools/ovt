// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.450 acceptance test 2: the main window has all 8 menus and the status bar. Also the toolbar, the disabled
// actions and the theme switch.

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Main"
    when: windowShown

    Component {
        id: mainComponent

        Main {
            visible: true
            title: "OVT Studio"
        }
    }

    function createMain() {
        const window = createTemporaryObject(mainComponent, null)
        verify(window !== null)
        return window
    }

    function test_menus() {
        const window = createMain()
        const titles = ["File", "Edit", "View", "Overlays", "Navigate", "Analysis", "Compare", "Help"]
        compare(window.menuBar.count, 8)
        for (let i = 0; i < titles.length; ++i)
            compare(window.menuBar.menuAt(i).title, titles[i])
    }

    function test_statusBarAndToolBar() {
        const window = createMain()
        verify(window.footer !== null)
        compare(window.footer.objectName, "statusBar")
        verify(window.footer.height > 0)
        compare(window.header.objectName, "toolBar")
        verify(window.header.height > 0)
        // The toolbar fits the minimum window width (§11.2.1: 1024 dip), so Jobs is never cut off.
        verify(window.header.contentItem.implicitWidth <= window.minimumWidth)
    }

    // Features that do not exist yet are visible but disabled; Quit and the themes work.
    function test_disabledActions() {
        const window = createMain()
        const file = window.menuBar.menuAt(0)
        compare(file.itemAt(0).text, "Open…")
        verify(!file.itemAt(0).enabled)
        compare(file.itemAt(file.count - 1).text, "Quit")
        verify(file.itemAt(file.count - 1).enabled)
    }

    function test_theme() {
        const window = createMain()
        Settings.theme = "dark"
        verify(OvtStyle.dark)
        compare(window.color.toString(), OvtStyle.window.toString())
        Settings.theme = "light"
        verify(!OvtStyle.dark)
        compare(Settings.theme, "light")
    }
}
