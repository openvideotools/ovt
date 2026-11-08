// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A detached panel group in its own window (chapter 11 §11.2.1, §11.2.2; WI-1.451). Desktop only: CMake leaves this
// file out of the Android build, so floating windows are compiled out there, not just hidden. Closing the window
// closes its panels; View ▸ Panels re-opens them docked.

import QtQuick
import QtQuick.Window
import OvtStudio

Window {
    id: window

    property var info: null
    property Item workspace: null

    function setup(groupInfo, dockWorkspace) {
        info = groupInfo
        workspace = dockWorkspace
        const main = dockWorkspace.Window.window
        x = main.x + (main.width - width) / 2
        y = main.y + (main.height - height) / 2
        visible = true
    }

    width: 480
    height: 360
    minimumWidth: 240
    minimumHeight: 160
    color: OvtStyle.window
    title: info ? DockModel.panelTitle(info.panelIds[info.currentIndex]) : ""
    onClosing: {
        if (info)
            for (const id of info.panelIds)
                DockModel.closePanel(id)
    }

    Loader {
        anchors.fill: parent
        active: window.workspace !== null
        sourceComponent: DockGroup {
            objectName: "dockGroup_floating"
            workspace: window.workspace
            areaName: "floating"
            groupIndex: window.info.group
            panelIds: window.info.panelIds
            currentIndex: window.info.currentIndex
        }
    }
}
