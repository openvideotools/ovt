// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Picture panel (chapter 11 §11.3.2): the WI-1.405 wiring (Welcome page without a file; with a file, the
// PictureHost view where the build has it and the "cannot show pictures" sentence where it does not) and the
// WI-1.456 input: wheel zoom about the cursor (AT1), Ctrl+click probe with its crosshair (AT2), keys 1–5 for the plane
// mode (AT3), pan, the selection rectangle, Esc and the shell's actions. The crosshair and the rectangle are drawn by
// the renderer (WI-2.436). Runs on the offscreen platform; the picture geometry is given to the controller directly
// (no decoder before WI-2.171). The rendering itself is tested in C++ (app/tests/picture, a real RHI), the
// controller's arithmetic in app/tests/models/test_picture_controller.cpp.

import QtQuick
import QtQuick.Window
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Picture"
    when: windowShown

    // A stand-in for the shell's Actions: the signal the panel listens to and what the Welcome page reads.
    QtObject {
        id: fakeActions

        signal activated(string id)

        function isEnabled(id) { return false }
        function shortcutText(id) { return "" }
        function trigger(id) {}
    }

    Component {
        id: panelComponent

        PicturePanel {
            // A top-level item has no window of its own, so it is hidden; the tests need it shown. keyClick() goes
            // to the active window, so the tests that type activate it.
            property Window stage: Window {
                width: 900
                height: 760
                visible: true
            }
            parent: stage.contentItem
            panelId: "picture"
            shellActions: fakeActions
            width: 900
            height: 760
        }
    }

    function init() {
        Project.closeAll()
        Selection.clearSelection("test")
    }
    function cleanup() {
        Project.closeAll()
        Selection.clearSelection("test")
    }

    function findByName(item, objectName) {
        if (item.objectName === objectName)
            return item
        // An index loop: a QQmlListProperty is not for-of iterable on Qt 6.4.
        for (let i = 0; i < item.children.length; ++i) {
            const found = findByName(item.children[i], objectName)
            if (found)
                return found
        }
        return null
    }

    // A panel with a file open and a 1920 x 1080 picture in its controller.
    function openPanel() {
        const panel = createTemporaryObject(panelComponent, testCase)
        verify(panel !== null)
        Project.openFile(Qt.resolvedUrl("tst_picture.qml"))
        compare(Project.fileCount, 1)
        tryVerify(function () { return findByName(panel, "pictureInput").visible })
        if (Platform.pictureHostAvailable)
            tryVerify(function () { return panel.host !== null })
        waitForRendering(panel)
        panel.controller.setPicture(1920, 1080, Qt.rect(0, 0, 0, 0))
        verify(panel.controller.hasPicture)
        return panel
    }

    // ---- WI-1.405 wiring ---------------------------------------------------------------------------------------

    function test_welcomeWithoutFile() {
        const panel = createTemporaryObject(panelComponent, testCase)
        verify(panel !== null)
        verify(findByName(panel, "welcomePage").visible)
        verify(!findByName(panel, "pictureViewLoader").active)
        verify(!findByName(panel, "pictureHeader").visible)
        compare(panel.host, null)
    }

    function test_pictureViewWithFile() {
        const panel = createTemporaryObject(panelComponent, testCase)
        Project.openFile(Qt.resolvedUrl("tst_picture.qml"))
        compare(Project.fileCount, 1)
        tryVerify(function () { return !findByName(panel, "welcomePage").visible })
        verify(findByName(panel, "pictureHeader").visible)
        const loader = findByName(panel, "pictureViewLoader")
        compare(loader.active, Platform.pictureHostAvailable)
        if (Platform.pictureHostAvailable) {
            tryVerify(function () { return panel.host !== null })
            compare(panel.host.objectName, "pictureHost")
            verify(!panel.host.hasPicture)  // nothing decoded yet: the §11.8 sentence shows
            compare(panel.host.zoom, 0)     // fit
        }
    }

    // ---- WI-1.456 ----------------------------------------------------------------------------------------------

    // AT1: Given a frame at zoom 1, When the wheel turns at device (400,300) so zoom becomes 2, Then the image point
    // under the cursor is unchanged (pick() before/after equal).
    function test_at1_wheelZoomKeepsPointUnderCursor() {
        const panel = openPanel()
        const input = findByName(panel, "pictureInput")
        const c = panel.controller
        c.zoomTo(1)
        compare(c.zoom, 1)
        const dpr = c.viewportWidth / input.width
        const x = 400 / dpr
        const y = 300 / dpr
        const before = c.pick(400, 300)
        verify(before.x >= 0)
        mouseWheel(input, x, y, 0, 120)
        compare(c.zoom, 2)
        compare(c.pick(400, 300), before)
        if (Platform.pictureHostAvailable) {
            compare(panel.host.zoom, 2)  // the host draws with the controller's ViewState
            compare(panel.host.panX, c.panX)
            compare(panel.host.panY, c.panY)
        }
    }

    // AT2: Given a frame shown, When the picture is Ctrl+clicked, Then Selection.probe is set and the crosshair drawn.
    // The crosshair is the renderer's overlay pass 12 (WI-2.436, issue #650): the panel hands the probe to the host,
    // and app/tests/picture/test_picture_host.cpp checks the drawn pixels.
    function test_at2_ctrlClickPinsProbeAndDrawsCrosshair() {
        const panel = openPanel()
        const input = findByName(panel, "pictureInput")
        const c = panel.controller
        c.zoomTo(1)
        const dpr = c.viewportWidth / input.width
        const expected = c.pick(250 * dpr, 180 * dpr)
        mouseClick(input, 250, 180, Qt.LeftButton, Qt.ControlModifier)
        compare(Selection.probe, expected)
        compare(Selection.origin, "picture")
        compare(c.probe, expected)
        verify(findByName(panel, "probeCrosshairH") === null)  // no scene-graph marker any more (FR-GUI-030)
        if (Platform.pictureHostAvailable)
            compare(panel.host.probe, expected)
        // Panning keeps the probe on its sample.
        c.panBy(-30 * dpr, 0)
        compare(Selection.probe, expected)
        // Ctrl+click on the pinned sample (now 30 px further left) unpins.
        mouseClick(input, 220, 180, Qt.LeftButton, Qt.ControlModifier)
        compare(Selection.probe, Qt.point(-1, -1))
        if (Platform.pictureHostAvailable)
            compare(panel.host.probe, Qt.point(-1, -1))
    }

    // AT3: Given the Picture focused, When keys 1–5 are pressed, Then ViewState.plane_mode reads 0–4.
    function test_at3_keysSelectPlanes() {
        const panel = openPanel()
        const input = findByName(panel, "pictureInput")
        panel.stage.requestActivate()
        tryVerify(function () { return panel.stage.active })
        input.forceActiveFocus()
        tryVerify(function () { return input.activeFocus })
        const keys = [Qt.Key_5, Qt.Key_4, Qt.Key_3, Qt.Key_2, Qt.Key_1]
        for (let i = 0; i < keys.length; ++i) {
            keyClick(keys[i])
            compare(panel.controller.planeMode, 4 - i)
            if (Platform.pictureHostAvailable)
                compare(panel.host.planeMode, 4 - i)
        }
        // The header's segmented control follows and sets the same state.
        mouseClick(findByName(panel, "plane_V"))
        compare(panel.controller.planeMode, 3)
        verify(findByName(panel, "plane_V").checked)
    }

    function test_shellActionsDriveZoomAndPlanes() {
        const panel = openPanel()
        const c = panel.controller
        verify(c.fit)
        fakeActions.activated("view.zoom.100")
        compare(c.zoom, 1)
        fakeActions.activated("view.zoom.in")
        compare(c.zoom, 2)
        fakeActions.activated("view.plane.gray")
        compare(c.planeMode, 4)
        fakeActions.activated("view.zoom.fit")
        verify(c.fit)
    }

    function test_dragPansAndDoubleClickFits() {
        const panel = openPanel()
        const input = findByName(panel, "pictureInput")
        const c = panel.controller
        c.zoomTo(1)
        const dpr = c.viewportWidth / input.width
        const panX = c.panX
        const panY = c.panY
        mouseDrag(input, 300, 300, 60, 40, Qt.LeftButton)
        verify(Math.abs(c.panX - (panX - 60 * dpr)) <= 1, "panX " + c.panX)
        verify(Math.abs(c.panY - (panY - 40 * dpr)) <= 1, "panY " + c.panY)
        verify(!c.panning)
        mouseDoubleClickSequence(input, 300, 300)
        verify(c.fit)
    }

    function test_shiftDragDrawsSelectionRectAndEscClears() {
        const panel = openPanel()
        const input = findByName(panel, "pictureInput")
        const c = panel.controller
        c.zoomTo(1)
        mouseDrag(input, 200, 200, 50, 30, Qt.LeftButton, Qt.ShiftModifier)
        const r = Selection.selectionRect
        verify(r.width > 0 && r.height > 0, "rect " + r)
        compare(r.x % 4, 0)
        compare(r.y % 4, 0)
        compare(r.width % 4, 0)
        if (Platform.pictureHostAvailable)
            compare(panel.host.selectionRect, r)  // drawn by the renderer (WI-2.436)
        panel.stage.requestActivate()
        tryVerify(function () { return panel.stage.active })
        input.forceActiveFocus()
        tryVerify(function () { return input.activeFocus })
        keyClick(Qt.Key_Escape)
        compare(Selection.selectionRect.width, 0)
        if (Platform.pictureHostAvailable)
            compare(panel.host.selectionRect.width, 0)
    }

    function test_hoverShowsReadout() {
        const panel = openPanel()
        const input = findByName(panel, "pictureInput")
        const c = panel.controller
        c.zoomTo(1)
        c.sampleReader = function (x, y) { return [x % 256, 128, 64] }
        panel.stage.requestActivate()
        tryVerify(function () { return panel.stage.active })
        mouseMove(input, 90, 90)
        mouseMove(input, 100, 100)
        const p = c.hover
        verify(p.x >= 0)
        const text = findByName(panel, "pictureReadout").text
        verify(text.indexOf("x " + p.x + ", y " + p.y) === 0, text)
        verify(text.indexOf("Y " + (p.x % 256) + " U 128 V 64") > 0, text)
        verify(text.indexOf("100 %") > 0, text)
    }
    // ---- Compare controls (WI-2.486) ------------------------------------------------------------------------------

    function test_compareComboNeedsB() {
        const panel = openPanel()
        const combo = findByName(panel, "compareCombo")
        verify(combo !== null)
        verify(!combo.enabled)
        panel.compare.setFileB(10)
        verify(combo.enabled)
        compare(combo.count, 7)
        panel.compare.nextMode()
        compare(combo.currentIndex, 1)
        verify(!findByName(panel, "compareDivider").visible)
    }

    function test_splitDividerTrackMovesTheDivider() {
        const panel = openPanel()
        panel.compare.setFileB(10)
        panel.compare.mode = CompareController.SplitVertical
        const divider = findByName(panel, "compareDivider")
        verify(divider.visible)
        const track = findByName(panel, "compareDividerTrack")
        mouseClick(track, divider.width * 0.25, 5)
        verify(Math.abs(panel.compare.splitPosition - 0.25) <= 0.01, "split " + panel.compare.splitPosition)
    }

    function test_cKeyCyclesWithPanelFocus() {
        const panel = openPanel()
        panel.compare.setFileB(10)
        panel.stage.requestActivate()
        tryVerify(function () { return panel.stage.active })
        const input = findByName(panel, "pictureInput")
        input.forceActiveFocus()
        tryVerify(function () { return input.activeFocus })
        const order = [1, 2, 3, 4, 5, 6]
        for (let i = 0; i < order.length; ++i) {
            keyClick(Qt.Key_C)
            compare(panel.compare.mode, order[i])
        }
        keyClick(Qt.Key_X)
        verify(panel.compare.swapped)
    }
}
