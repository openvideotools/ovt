// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-A.451 acceptance test 1, desktop half: the layouts by width of §11.5.2 switch live as `Platform.widthDp`
// changes, without losing state. `Platform.widthDp` is writable, so the test stands in for the Android Configuration
// (platform_android.cpp) by setting it; the shell is created with `compactEnabled` as the Android build has it. The
// emulator half is test_android_breakpoints.py and docs/acceptance/wi/WI-A.451.md.

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Breakpoints"
    when: windowShown

    Window {
        id: host

        width: 1300
        height: 800
        visible: true
    }

    Component {
        id: shellComponent

        AppShell {
            width: 1300
            height: 800
            compactEnabled: true
        }
    }

    function initTestCase() {
        testSetup.attachSelection()
    }

    function init() {
        Project.closeAll()
        Platform.widthDp = 1300
    }

    function cleanup() {
        Platform.widthDp = 1280
    }

    function createShell() {
        const shell = createTemporaryObject(shellComponent, host.contentItem)
        verify(shell !== null)
        return shell
    }

    function shown(item) {
        return item !== null && item.visible && item.width > 0 && item.height > 0
    }

    function test_layoutClassTable() {
        compare(Platform.layoutClassForWidth(1920), "three")
        compare(Platform.layoutClassForWidth(1200), "three")
        compare(Platform.layoutClassForWidth(1199), "two")
        compare(Platform.layoutClassForWidth(840), "two")
        compare(Platform.layoutClassForWidth(839), "vertical")
        compare(Platform.layoutClassForWidth(600), "vertical")
        compare(Platform.layoutClassForWidth(599), "single")
        compare(Platform.layoutClassForWidth(320), "single")
    }

    // The window's width, when given, is the width (a stale Configuration read must not hold the old class).
    function test_refreshTakesTheWindowWidth() {
        Platform.refresh(700)
        compare(Platform.widthDp, 700)
        Platform.refresh(0)
        compare(Platform.widthDp, 700)
    }

    // The desktop never compacts: the same width on a desktop shell stays three-pane.
    function test_desktopStaysThreePane() {
        const shell = createTemporaryObject(shellComponent, host.contentItem, {"compactEnabled": false})
        Platform.widthDp = 700
        compare(shell.layoutClass, "three")
        verify(!shown(findChild(shell, "compactBar")))
    }

    // Acceptance test 1: 1300 -> 900 -> 700 dp.
    function test_resizeSwitchesLayoutsKeepingSelection() {
        const shell = createShell()
        const ws = shell.dockWorkspace
        const picture = ws.panelItem("picture")
        Selection.selectFrame(42, -1, "test")
        compare(Selection.frameDecodeIndex, 42)

        compare(shell.layoutClass, "three")
        verify(!ws.compact)
        verify(ws.dragEnabled)
        verify(!shown(findChild(shell, "compactBar")))
        compare(ws.splitRoot.orientation, Qt.Horizontal)

        Platform.widthDp = 900
        compare(shell.layoutClass, "two")
        verify(ws.compact)
        verify(!ws.dragEnabled)
        tryVerify(function () { return shown(findChild(shell, "compactBar")) })
        verify(shown(findChild(shell, "drawerButton")))
        verify(!shown(findChild(shell, "bottomNav")))
        compare(Selection.frameDecodeIndex, 42)

        Platform.widthDp = 700
        compare(shell.layoutClass, "vertical")
        verify(ws.stacked)
        compare(ws.splitRoot.orientation, Qt.Vertical)
        compare(Selection.frameDecodeIndex, 42)
        // The picture panel and its state survive the switches: the same item, not a rebuilt one.
        verify(ws.panelItem("picture") === picture)

        // Back up: three-pane again, drag-and-drop on, the drawer closed.
        ws.drawerOpen = true
        Platform.widthDp = 1300
        compare(shell.layoutClass, "three")
        verify(!ws.drawerOpen)
        verify(ws.dragEnabled)
        compare(Selection.frameDecodeIndex, 42)
    }

    function test_twoPanePicture60PercentAndCompactTimeline() {
        const shell = createShell()
        Platform.widthDp = 900
        const ws = shell.dockWorkspace
        const right = findChild(shell, "dockArea_right")
        const bottom = findChild(shell, "dockArea_bottom")
        // Picture 60 %, tabbed area 40 % of the workspace; the timeline is the 120 dp strip.
        tryVerify(function () { return right.visible && Math.abs(right.width / ws.width - 0.4) < 0.02 },
                  5000, "right " + right.width + " of " + ws.width)
        if (bottom.visible)
            tryCompare(bottom, "height", 120)
    }

    function test_verticalStacksPictureTimelineAndTabs() {
        const shell = createShell()
        Platform.widthDp = 700
        const center = findChild(shell, "dockGroup_center")
        const right = findChild(shell, "dockArea_right")
        const bottom = findChild(shell, "dockArea_bottom")
        tryVerify(function () { return right.visible && right.height > 0 && right.width === shell.dockWorkspace.width })
        const top = center.mapToItem(null, 0, 0).y
        const mid = bottom.visible ? bottom.mapToItem(null, 0, 0).y : top + center.height
        const low = right.mapToItem(null, 0, 0).y
        verify(top < mid && mid <= low, "picture " + top + " / timeline " + mid + " / tabs " + low)
        if (bottom.visible)
            compare(bottom.height, 96)
        // Full width: the areas are stacked, not side by side.
        compare(right.width, shell.dockWorkspace.width)
    }

    // < 600 dp: a single pane with bottom navigation; it must not crash and every entry shows one area.
    function test_singlePaneBottomNavigation() {
        const shell = createShell()
        Platform.widthDp = 500
        compare(shell.layoutClass, "single")
        const nav = findChild(shell, "bottomNav")
        tryVerify(function () { return shown(nav) })
        compare(nav.count, 4)
        const center = findChild(shell, "dockGroup_center")
        const right = findChild(shell, "dockArea_right")
        tryVerify(function () { return shown(center) })
        verify(!right.visible)
        nav.currentIndex = 2
        tryVerify(function () { return right.visible && right.width > 0 })
        verify(!center.visible)
        nav.currentIndex = 0
        tryVerify(function () { return shown(center) })
        verify(!right.visible)
        Platform.widthDp = 1300
        tryVerify(function () { return shown(center) && right.visible })
    }

    function test_drawerButtonTogglesLeftArea() {
        const shell = createShell()
        Platform.widthDp = 900
        const ws = shell.dockWorkspace
        verify(!ws.drawerOpen)
        findChild(shell, "drawerButton").clicked()
        verify(ws.drawerOpen)
        findChild(shell, "drawerButton").clicked()
        verify(!ws.drawerOpen)
    }
}
