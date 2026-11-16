// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The picture area of the Picture panel (chapter 11 §11.3.2; WI-1.405): the PictureHost item over the whole panel,
// with the §11.8 empty-state sentence while nothing is decoded and the renderer's error when it has one. Only part of
// the module where PictureHost exists (app/CMakeLists.txt); PicturePanel.qml loads it by URL.

import QtQuick
import QtQuick.Controls
import OvtStudio

Item {
    id: view

    readonly property alias host: host

    PictureHost {
        id: host

        objectName: "pictureHost"
        anchors.fill: parent
    }

    EmptyState {
        id: emptyState

        anchors.fill: parent
        visible: !host.hasPicture || host.lastError !== ""
        text: host.lastError !== "" ? host.lastError : emptyState.textFor("picture")
    }
}
