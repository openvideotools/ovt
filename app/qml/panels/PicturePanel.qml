// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Picture panel (chapter 11 §11.3.2). WI-1.405 wires the EMBEDDED-mode picture: while a file is open it loads
// PictureView.qml (the PictureHost item, chapter 10 §10.1.2), which exists only in builds that have PictureHost
// (Platform.pictureHostAvailable: Qt >= 6.7 with libovt-render, desktop). With no file open the panel is the Welcome
// page (§11.3.19). Zoom/pan, planes, probe and selection input arrive with WI-1.456.

import QtQuick
import QtQuick.Controls
import OvtStudio

Rectangle {
    id: panel

    objectName: "picturePanel"

    required property string panelId
    // The shell's Actions, for the Welcome page's buttons; null in tests that build the panel alone.
    property var actions: null
    // The PictureHost item once PictureView.qml is loaded, else null (WI-1.456 binds its ViewState here).
    readonly property var host: pictureView.item ? pictureView.item.host : null

    color: OvtStyle.panel
    Accessible.name: DockModel.panelTitle(panelId)

    WelcomePage {
        anchors.fill: parent
        visible: Project.fileCount === 0
        actions: panel.actions
    }

    Loader {
        id: pictureView

        objectName: "pictureViewLoader"
        anchors.fill: parent
        active: Platform.pictureHostAvailable && Project.fileCount > 0
        visible: active
        source: "PictureView.qml"
    }

    EmptyState {
        anchors.fill: parent
        visible: Project.fileCount > 0 && !Platform.pictureHostAvailable
        text: qsTr("This build cannot show pictures: it needs Qt 6.7 or later and the renderer.")
    }
}
