// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dock manager in the main window (WI-1.451; chapter 11 §11.2.2). Acceptance test 2: given preset Analyze, when
// a tab is dragged to the right edge, then a new Right group exists with that panel. Also presets from the View menu,
// tabs, closing and re-opening panels, Picture only and the keyboard shortcuts.

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Dock"
    when: windowShown

    Component {
        id: mainComponent

        Main {
            visible: true
            width: 1280
            height: 800
        }
    }

    function init() {
        DockModel.resetLayout()
    }

    function createMain() {
        const window = createTemporaryObject(mainComponent, null)
        verify(window !== null)
        waitForRendering(window.contentItem)
        return window
    }

    function rightPanels() {
        return DockModel.groups("right").map(g => g.panelIds.join(","))
    }

    // Like findChild, but through the visual tree: the Repeater-made tabs and groups are visual children only.
    function find(item, name) {
        if (item.objectName === name)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = find(item.children[i], name)
            if (found !== null)
                return found
        }
        return null
    }

    function test_analyzeAreas() {
        const window = createMain()
        compare(DockModel.layoutName, "Analyze")
        compare(rightPanels(), ["syntax", "selection_info", "block_samples"])
        verify(find(window.contentItem, "dockArea_left").visible)
        verify(find(window.contentItem, "dockArea_right").visible)
        verify(find(window.contentItem, "dockArea_bottom").visible)
        verify(find(window.contentItem, "dockTab_picture") !== null)
        verify(find(window.contentItem, "dockTab_hex") !== null)
    }

    // Acceptance test 2.
    function test_dragTabToRightEdge() {
        const window = createMain()
        const tab = find(window.contentItem, "dockTab_hex")
        verify(tab !== null)
        const workspace = find(window.contentItem, "dockWorkspace")
        const start = tab.mapToItem(window.contentItem, tab.width / 2, tab.height / 2)
        const end = workspace.mapToItem(window.contentItem, workspace.width - 8, workspace.height / 3)
        const before = DockModel.groups("right").length
        mousePress(window.contentItem, start.x, start.y)
        const steps = 12
        for (let i = 1; i <= steps; ++i)
            mouseMove(window.contentItem, start.x + (end.x - start.x) * i / steps, start.y + (end.y - start.y) * i / steps)
        compare(workspace.dropIndicator.target.kind, "group")
        compare(workspace.dropIndicator.target.area, "right")
        verify(find(window.contentItem, "dockDropHighlight").visible)
        mouseRelease(window.contentItem, end.x, end.y)
        compare(DockModel.groups("right").length, before + 1)
        const groups = DockModel.groups("right")
        compare(groups[groups.length - 1].panelIds, ["hex"])
        tryVerify(() => !find(window.contentItem, "dockDropHighlight").visible)
    }

    function test_dropTargets() {
        const window = createMain()
        const workspace = find(window.contentItem, "dockWorkspace")
        const indicator = workspace.dropIndicator
        const center = find(window.contentItem, "dockGroup_center")
        const mid = center.mapToItem(null, center.width / 2, center.height / 2)
        compare(indicator.targetAt(mid).kind, "tab")
        compare(indicator.targetAt(mid).group, center.groupIndex)
        const left = workspace.mapToItem(null, 4, workspace.height / 2)
        compare(indicator.targetAt(left).area, "left")
        const bottom = workspace.mapToItem(null, workspace.width / 2, workspace.height - 4)
        compare(indicator.targetAt(bottom).area, "bottom")
        // The top quarter of the first Right group: a new group before it.
        const syntax = find(window.contentItem, "dockGroup_right_0")
        const top = syntax.mapToItem(null, syntax.width / 2, syntax.height / 8)
        compare(indicator.targetAt(top).kind, "group")
        compare(indicator.targetAt(top).position, 0)
        // Outside the window: float on the desktop.
        const outside = workspace.mapToItem(null, workspace.width / 2, -50)
        compare(indicator.targetAt(outside) !== null, DockModel.floatingSupported)

        // Dropping the Picture outside Center is refused and changes nothing.
        indicator.begin("picture", left)
        verify(!indicator.finish())
        compare(DockModel.groups("center")[0].panelIds, ["picture"])
    }

    // Detach (desktop only): the panel moves to a floating window; closing the window closes the panel.
    function test_floating() {
        if (!DockModel.floatingSupported)
            skip("no floating windows on this platform")
        const window = createMain()
        verify(DockModel.floatPanel("hex"))
        compare(DockModel.areas.floating.length, 1)
        tryVerify(() => {
            const g = find(window.contentItem, "dockTab_hex")
            return g === null
        })
        const workspace = find(window.contentItem, "dockWorkspace")
        tryVerify(() => workspace.panelItem("hex").Window.window !== window)
        verify(DockModel.closePanel("hex"))
        compare(DockModel.areas.floating.length, 0)
    }

    function test_tabsAndClose() {
        const window = createMain()
        const center = DockModel.groups("center")[0].group
        verify(DockModel.movePanelToGroup("ref_graph", center, 1) === false) // not open yet
        verify(DockModel.openPanel("ref_graph"))
        verify(DockModel.movePanelToGroup("ref_graph", DockModel.groupOf("picture"), 1))
        tryVerify(() => find(window.contentItem, "dockTab_ref_graph") !== null)
        compare(DockModel.groups("center")[0].panelIds, ["picture", "ref_graph"])
        // Clicking the Picture tab makes it current.
        mouseClick(find(window.contentItem, "dockTab_picture"))
        compare(DockModel.groups("center")[0].currentIndex, 0)
        verify(DockModel.closePanel("ref_graph"))
        tryVerify(() => find(window.contentItem, "dockTab_ref_graph") === null)
    }

    function test_viewMenu() {
        const window = createMain()
        const view = window.menuBar.menuAt(2)
        const layout = view.itemAt(0).subMenu
        compare(layout.title, "Layout")
        compare(layout.itemAt(1).text, "Syntax")
        verify(layout.itemAt(1).enabled)
        layout.itemAt(1).action.trigger()
        compare(DockModel.layoutName, "Syntax")
        tryVerify(() => !find(window.contentItem, "dockArea_left").visible)
        compare(view.itemAt(1).text, "Reset Layout")
        view.itemAt(1).action.trigger()
        compare(DockModel.layoutName, "Analyze")

        const panels = view.itemAt(2).subMenu
        compare(panels.title, "Panels")
        compare(panels.count, DockModel.allPanels().length)
        let hex = null
        for (let i = 0; i < panels.count; ++i)
            if (panels.itemAt(i).text === "Hex")
                hex = panels.itemAt(i)
        verify(hex !== null && hex.checked)
        hex.action.trigger()
        verify(DockModel.openPanels.indexOf("hex") < 0)
        verify(!hex.checked)
        verify(DockModel.closePanel("timeline"))
        hex.action.trigger()
        verify(hex.checked)
        tryVerify(() => find(window.contentItem, "dockTab_hex") !== null)
    }

    function test_shortcuts() {
        const window = createMain()
        const workspace = find(window.contentItem, "dockWorkspace")
        window.requestActivate()
        tryVerify(() => window.active)
        // Ctrl+Shift+F: Picture only.
        keyClick(Qt.Key_F, Qt.ControlModifier | Qt.ShiftModifier)
        verify(DockModel.pictureOnly)
        tryVerify(() => !find(window.contentItem, "dockArea_right").visible)
        keyClick(Qt.Key_F, Qt.ControlModifier | Qt.ShiftModifier)
        verify(!DockModel.pictureOnly)
        // Ctrl+Shift+L / R / B toggle the areas.
        keyClick(Qt.Key_L, Qt.ControlModifier | Qt.ShiftModifier)
        verify(!DockModel.leftVisible)
        keyClick(Qt.Key_L, Qt.ControlModifier | Qt.ShiftModifier)
        verify(DockModel.leftVisible)
        keyClick(Qt.Key_R, Qt.ControlModifier | Qt.ShiftModifier)
        verify(!DockModel.rightVisible)
        keyClick(Qt.Key_B, Qt.ControlModifier | Qt.ShiftModifier)
        verify(!DockModel.bottomVisible)
        // Ctrl+Shift+5: the fifth panel in tab order (Selection Info), then Ctrl+Tab cycles its group.
        DockModel.resetLayout()
        verify(DockModel.movePanelToGroup("units", DockModel.groupOf("selection_info"), 1))
        keyClick(Qt.Key_5, Qt.ControlModifier | Qt.ShiftModifier)
        compare(workspace.focusedPanel, DockModel.tabOrder()[4])
        workspace.focusPanel("selection_info")
        keyClick(Qt.Key_Tab, Qt.ControlModifier)
        compare(workspace.focusedPanel, "units")
        keyClick(Qt.Key_Tab, Qt.ControlModifier)
        compare(workspace.focusedPanel, "selection_info")
    }
}
