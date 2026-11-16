// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Picture panel (chapter 11 §11.3.2) in EMBEDDED mode (WI-1.456): plane selector and Stage combo in the header,
// the picture (PictureView.qml: the PictureHost item of WI-1.405, chapter 10 §10.1.2) with the panel's input over
// it, and the status strip with the probe read-out. With no file open the panel is the Welcome page (§11.3.19).
//
// PictureController owns the ViewState and turns input into ViewState and Selection changes (gesture table
// §11.4.4); this file only routes events to it. Compare modes (WI-1.458) and overlays (WI-2.481) come later. The
// window-scope keys (1–5 planes, + − R Ctrl+1/2/4 zoom) arrive through the shell's Actions; the same keys pressed
// while the panel has focus and no window shortcut took them (a panel built alone, as in the tests) go through the
// Keymap here.

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import OvtStudio

Rectangle {
    id: panel

    objectName: "picturePanel"

    required property string panelId
    // The shell's Actions, for the Welcome page's buttons and the window-scope Picture actions; null in tests that
    // build the panel alone.
    property var shellActions: null
    // The PictureHost item once PictureView.qml is loaded, else null.
    readonly property var host: pictureView.item ? pictureView.item.host : null
    readonly property alias controller: controller
    readonly property bool showsPicture: Project.fileCount > 0

    color: OvtStyle.panel
    Accessible.name: DockModel.panelTitle(panelId)

    PictureController {
        id: controller

        selection: Selection
        origin: panel.panelId
        viewportWidth: canvas.width * canvas.dpr
        viewportHeight: canvas.height * canvas.dpr
        // [Y, U, V] code values at a luma sample, read from the frame the host shows; [] without one.
        sampleReader: function (x, y) { return panel.host ? panel.host.codeValues(x, y) : [] }
    }

    Connections {
        target: panel.shellActions
        ignoreUnknownSignals: true

        function onActivated(id) {
            if (panel.showsPicture)
                controller.handleAction(id)
        }
    }

    WelcomePage {
        anchors.fill: parent
        visible: !panel.showsPicture
        actions: panel.shellActions
    }

    // ---- Header: plane selector and Stage combo (§11.3.2) ----------------------------------------------------------
    Rectangle {
        id: header

        objectName: "pictureHeader"
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: visible ? OvtStyle.minHitTarget + 4 : 0
        visible: panel.showsPicture
        color: OvtStyle.window

        Row {
            anchors { right: parent.right; rightMargin: OvtStyle.spacing; verticalCenter: parent.verticalCenter }
            spacing: OvtStyle.spacing

            Row {
                objectName: "planeSelector"
                spacing: 0

                Repeater {
                    model: ["YUV", "Y", "U", "V", "Gray"]

                    delegate: Button {
                        required property string modelData
                        required property int index

                        objectName: "plane_" + modelData
                        text: modelData
                        checkable: true
                        checked: controller.planeMode === index
                        flat: !checked
                        focusPolicy: Qt.NoFocus
                        implicitHeight: OvtStyle.minHitTarget
                        implicitWidth: Math.max(OvtStyle.minHitTarget, implicitContentWidth + 12)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Plane %1 (%2)").arg(modelData).arg(index + 1)
                        onClicked: controller.planeMode = index
                    }
                }
            }

            ComboBox {
                objectName: "stageCombo"
                visible: controller.stageAvailable
                implicitHeight: OvtStyle.minHitTarget
                focusPolicy: Qt.NoFocus
                model: [qsTr("Stage: Recon"), qsTr("Stage: Prediction"), qsTr("Stage: Residual"),
                        qsTr("Stage: Pre-deblock"), qsTr("Stage: Pre-SAO/CDEF"), qsTr("Stage: Pre-LR")]
                currentIndex: controller.stage
                onActivated: function (index) { controller.stage = index }
            }
        }
    }

    // ---- Canvas: the picture and its input ---------------------------------------------------------------------------
    Item {
        id: canvas

        objectName: "pictureCanvas"
        // Device pixels per item pixel: the controller works in the host's (device) pixels.
        readonly property real dpr: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1

        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: statusStrip.top }
        visible: panel.showsPicture
        clip: true

        Loader {
            id: pictureView

            objectName: "pictureViewLoader"
            anchors.fill: parent
            active: Platform.pictureHostAvailable && panel.showsPicture
            visible: active
            source: "PictureView.qml"
            onLoaded: item.controller = controller
        }

        EmptyState {
            anchors.fill: parent
            visible: !Platform.pictureHostAvailable
            text: qsTr("This build cannot show pictures: it needs Qt 6.7 or later and the renderer.")
        }

        // SPEC-ASSUMPTION(#650): the probe crosshair and the selection rectangle belong to libovt-render
        // (OverlayLayer::Probe / SelectionRect, chapter 10 §10.5.2 rows 17–18, WI-2.436, phase 2). Until that pass
        // exists the panel draws these two thin markers as items, in the §10.5.2 colours; WI-2.436 removes them and
        // passes OverlayConfig.probe_x/y and selection_rect instead. Video and block overlays stay in the renderer.
        Item {
            id: markers

            objectName: "pictureMarkers"
            anchors.fill: parent
            // The renderer's mapping (PictureController::devicePoint), written out over the controller's
            // properties so that the bindings track zoom and pan (a comma expression's dependencies are not
            // tracked by compiled bindings on every Qt version).
            function devX(ix) {
                return ((ix - controller.panX) * controller.zoom + controller.viewportWidth / 2) / canvas.dpr
            }
            function devY(iy) {
                return ((iy - controller.panY) * controller.zoom + controller.viewportHeight / 2) / canvas.dpr
            }
            readonly property bool hasProbe: controller.hasPicture && controller.probe.x >= 0
            readonly property rect shownRect: controller.dragRect.width > 0 ? controller.dragRect
                                                                             : controller.selectionRect

            Rectangle {
                objectName: "probeCrosshairH"
                visible: markers.hasProbe
                color: "#FF4136"
                x: 0
                width: parent.width
                height: 1
                y: markers.hasProbe ? Math.floor(markers.devY(controller.probe.y + 0.5)) : 0
            }
            Rectangle {
                objectName: "probeCrosshairV"
                visible: markers.hasProbe
                color: "#FF4136"
                y: 0
                height: parent.height
                width: 1
                x: markers.hasProbe ? Math.floor(markers.devX(controller.probe.x + 0.5)) : 0
            }
            Rectangle {
                id: rectMarker

                objectName: "selectionRectMarker"
                readonly property real x0: markers.devX(markers.shownRect.x)
                readonly property real y0: markers.devY(markers.shownRect.y)

                visible: markers.shownRect.width > 0 && markers.shownRect.height > 0
                x: x0
                y: y0
                width: markers.devX(markers.shownRect.x + markers.shownRect.width) - x0
                height: markers.devY(markers.shownRect.y + markers.shownRect.height) - y0
                color: "#3339CCCC"
                border.color: "#39CCCC"
                border.width: 1
            }
        }

        // The gesture table of §11.4.4 for the mouse. Touch (pinch, long-press) is WI-A.450's.
        MouseArea {
            id: input

            objectName: "pictureInput"
            property bool spaceHeld: false
            property int mode: 0  // 0 none, 1 pan, 2 rectangle, 3 click (no drag yet)
            property point pressAt

            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            hoverEnabled: true
            focus: true
            activeFocusOnTab: true
            cursorShape: mode === 1 ? Qt.ClosedHandCursor : (spaceHeld ? Qt.OpenHandCursor : Qt.CrossCursor)
            Accessible.role: Accessible.Graphic
            Accessible.name: qsTr("Picture")

            function dx(v) { return v * canvas.dpr }

            onPressed: function (mouse) {
                forceActiveFocus()
                pressAt = Qt.point(mouse.x, mouse.y)
                if (!controller.hasPicture) {
                    mode = 0
                } else if (mouse.button === Qt.MiddleButton
                           || (mouse.button === Qt.LeftButton && spaceHeld)) {
                    mode = 1
                    controller.beginPan(dx(mouse.x), dx(mouse.y))
                } else if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ShiftModifier)) {
                    mode = 2
                    controller.beginRect(dx(mouse.x), dx(mouse.y))
                } else if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ControlModifier)) {
                    mode = 0
                    controller.toggleProbe(dx(mouse.x), dx(mouse.y))
                } else if (mouse.button === Qt.LeftButton) {
                    // A left drag on an empty area pans; a click selects a block once FrameAnalysis overlays exist.
                    mode = 3
                } else {
                    mode = 0
                }
            }
            onPositionChanged: function (mouse) {
                controller.hoverAt(dx(mouse.x), dx(mouse.y))
                if (mode === 3 && (Math.abs(mouse.x - pressAt.x) + Math.abs(mouse.y - pressAt.y)) >= 3) {
                    mode = 1
                    controller.beginPan(dx(pressAt.x), dx(pressAt.y))
                }
                if (mode === 1)
                    controller.movePan(dx(mouse.x), dx(mouse.y))
                else if (mode === 2)
                    controller.moveRect(dx(mouse.x), dx(mouse.y))
            }
            onReleased: function (mouse) {
                if (mode === 1)
                    controller.endPan()
                else if (mode === 2)
                    controller.endRect()
                mode = 0
            }
            onCanceled: {
                controller.endPan()
                controller.escape()
                mode = 0
            }
            onExited: controller.leave()
            onDoubleClicked: function (mouse) {
                if (!(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)))
                    controller.fitToView()
            }
            onWheel: function (wheel) {
                controller.wheel(dx(wheel.x), dx(wheel.y), wheel.angleDelta.y)
                wheel.accepted = true
            }

            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_Space && !event.isAutoRepeat) {
                    spaceHeld = true
                    event.accepted = true
                    return
                }
                const windowActions = ["view.plane.yuv", "view.plane.y", "view.plane.u", "view.plane.v",
                                       "view.plane.gray", "view.zoom.in", "view.zoom.out", "view.zoom.fit",
                                       "view.zoom.100", "view.zoom.200", "view.zoom.400"]
                for (const id of windowActions) {
                    if (Keymap.matches("action." + id, event.key, event.modifiers)) {
                        event.accepted = controller.handleAction(id)
                        return
                    }
                }
                const moves = {"picture.probeLeft": [-1, 0], "picture.probeRight": [1, 0],
                               "picture.probeUp": [0, -1], "picture.probeDown": [0, 1]}
                for (const id in moves) {
                    if (Keymap.matches("action." + id, event.key, event.modifiers)) {
                        controller.moveProbe(moves[id][0], moves[id][1])
                        event.accepted = true
                        return
                    }
                }
                if (Keymap.matches("action.focus.escape", event.key, event.modifiers))
                    event.accepted = controller.escape()
            }
            Keys.onReleased: function (event) {
                if (event.key === Qt.Key_Space && !event.isAutoRepeat) {
                    spaceHeld = false
                    event.accepted = true
                }
            }
        }
    }

    // ---- Status strip: probe read-out, zoom and plane (§11.3.2) ------------------------------------------------------
    Rectangle {
        id: statusStrip

        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: visible ? readout.implicitHeight + 6 : 0
        visible: panel.showsPicture
        color: OvtStyle.window

        Label {
            id: readout

            objectName: "pictureReadout"
            anchors { left: parent.left; leftMargin: OvtStyle.spacing; verticalCenter: parent.verticalCenter }
            text: controller.readout
            color: OvtStyle.textMuted
            font.pixelSize: OvtStyle.fontSize - 1
            font.family: "monospace"
        }
    }
}
