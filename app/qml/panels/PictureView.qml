// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The picture area of the Picture panel (chapter 11 §11.3.2; WI-1.405, WI-1.456): the PictureHost item over the
// whole canvas, drawing with the ViewState of the panel's PictureController, with the §11.8 empty-state sentence
// while nothing is decoded and the renderer's error when it has one. Only part of the module where PictureHost exists
// (app/CMakeLists.txt); PicturePanel.qml loads it by URL.
//
// NATIVE mode (WI-1.463, §11.2.1): the NativeSurfaceHost below puts a child window over the PictureHost while HDR
// output is requested (HdrView.hdrOutput: View ▸ HDR output, Ctrl+H, the toolbar and HDR panel toggles) on an
// HDR-capable display, and hides it while a popup is open over the picture (§11.5.5 item 3). The host follows it.

import QtQuick
import QtQuick.Controls
import OvtStudio

Item {
    id: view

    // Set by PicturePanel when loaded; null leaves the host on its defaults (fit, YUV).
    property PictureController controller: null
    // The panel's CompareController (WI-2.486); null leaves the host on Single.
    property CompareController compare: null
    readonly property alias host: host

    PictureHost {
        id: host

        objectName: "pictureHost"
        anchors.fill: parent
        // zoom 0 is the host's own fit rule, which the controller mirrors (PictureController::refit()).
        zoom: view.controller && !view.controller.fit ? view.controller.zoom : 0
        panX: view.controller ? view.controller.panX : 0
        panY: view.controller ? view.controller.panY : 0
        planeMode: view.controller ? view.controller.planeMode : 0
        showCropped: view.controller ? view.controller.showCropped : true
        // The pinned probe and the selection rectangle (the one being dragged, else the committed one) are drawn by
        // the renderer's overlay passes (chapter 10 §10.5.6 pass 12, WI-2.436, issue #650).
        probe: view.controller && view.controller.hasPicture ? view.controller.probe : Qt.point(-1, -1)
        selectionRect: !view.controller ? Qt.rect(0, 0, 0, 0)
                       : (view.controller.dragRect.width > 0 ? view.controller.dragRect : view.controller.selectionRect)
        // The HDR fields of the ViewState come from the HDR panel's Display tab (WI-1.462).
        toneMap: HdrView.toneMap
        gamutMap: HdrView.gamutMap
        clipNits: HdrView.clipNits
        targetPeakOverride: HdrView.targetPeakOverride
        falseColor: HdrView.falseColor
        gamutClipOverlay: HdrView.gamutClipOverlay
        dither: HdrView.dither
        nativeHost: nativeHost

        compareKind: view.compare ? view.compare.kind : 0
        splitAxis: view.compare ? view.compare.splitAxis : 0
        splitPosition: view.compare ? view.compare.splitPosition : 0.5
        overlayAlpha: view.compare ? view.compare.overlayAlpha : 0.5
        diffGain: view.compare ? view.compare.diffGain : 1
        diffFalseColour: view.compare ? view.compare.diffFalseColour : false
        flickerPeriodMs: view.compare ? view.compare.flickerPeriodMs : 500

        onPictureChanged: view.pushPicture()
        // The Display Capabilities panel and the status bar show the encoding in use ("Srgb8" while EMBEDDED).
        onOutputEncodingChanged: DisplayCaps.outputEncoding = host.outputEncoding
    }

    NativeSurfaceHost {
        id: nativeHost

        item: host
        overlay: view.Overlay.overlay
        requested: HdrView.hdrOutput
        hdrCapable: DisplayCaps.hdrCapable
        headroom: DisplayCaps.headroom
        encoding: HdrView.macEncoding
    }

    // The controller asks, the host does: snapshot A into slot B, swap, clear.
    Connections {
        target: view.compare
        ignoreUnknownSignals: true

        function onSnapshotRequested() { view.compare.snapshotTaken(host.snapshotAsB()) }
        function onSwapRequested() { host.swapAB() }
        function onBChanged() {
            if (view.compare.bSource === "none")
                host.clearB()
        }
    }

    function pushPicture() {
        if (view.controller)
            view.controller.setPicture(host.pictureWidth, host.pictureHeight, host.crop)
    }

    onControllerChanged: pushPicture()

    EmptyState {
        id: emptyState

        anchors.fill: parent
        visible: !host.hasPicture || host.lastError !== ""
        text: host.lastError !== "" ? host.lastError : emptyState.textFor("picture")
    }
}
