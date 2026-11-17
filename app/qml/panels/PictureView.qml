// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The picture area of the Picture panel (chapter 11 §11.3.2; WI-1.405, WI-1.456): the PictureHost item over the
// whole canvas, drawing with the ViewState of the panel's PictureController, with the §11.8 empty-state sentence
// while nothing is decoded and the renderer's error when it has one. Only part of the module where PictureHost exists
// (app/CMakeLists.txt); PicturePanel.qml loads it by URL.

import QtQuick
import QtQuick.Controls
import OvtStudio

Item {
    id: view

    // Set by PicturePanel when loaded; null leaves the host on its defaults (fit, YUV).
    property PictureController controller: null
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
        // The HDR fields of the ViewState come from the HDR panel's Display tab (WI-1.462).
        toneMap: HdrView.toneMap
        gamutMap: HdrView.gamutMap
        clipNits: HdrView.clipNits
        targetPeakOverride: HdrView.targetPeakOverride
        falseColor: HdrView.falseColor
        gamutClipOverlay: HdrView.gamutClipOverlay
        dither: HdrView.dither

        onPictureChanged: view.pushPicture()
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
