// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.405 Picture panel wiring: the Welcome page while no file is open; with a file open, the PictureHost view where
// the build has it (Platform.pictureHostAvailable) and the "cannot show pictures" sentence where it does not. The
// rendering itself is tested in C++ (app/tests/picture, a real RHI); this runs on the offscreen platform.

import QtQuick
import QtQuick.Window
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "PicturePanel"
    when: windowShown

    Component {
        id: panelComponent

        PicturePanel {
            // A top-level item has no window of its own, so it is hidden; the tests need it shown.
            property Window stage: Window {
                width: 320
                height: 180
                visible: true
            }
            parent: stage.contentItem
            panelId: "picture"
            width: 320
            height: 180
        }
    }

    function init() {
        Project.closeAll()
    }
    function cleanup() {
        Project.closeAll()
    }

    function findByName(item, objectName) {
        if (item.objectName === objectName)
            return item
        for (const kid of item.children) {
            const found = findByName(kid, objectName)
            if (found)
                return found
        }
        return null
    }

    function test_welcomeWithoutFile() {
        const panel = createTemporaryObject(panelComponent, testCase)
        verify(panel !== null)
        verify(findByName(panel, "welcomePage").visible)
        verify(!findByName(panel, "pictureViewLoader").active)
        compare(panel.host, null)
    }

    function test_pictureViewWithFile() {
        const panel = createTemporaryObject(panelComponent, testCase)
        Project.openFile(Qt.resolvedUrl("tst_picture_panel.qml"))
        compare(Project.fileCount, 1)
        tryVerify(function () { return !findByName(panel, "welcomePage").visible })
        const loader = findByName(panel, "pictureViewLoader")
        compare(loader.active, Platform.pictureHostAvailable)
        if (Platform.pictureHostAvailable) {
            tryVerify(function () { return panel.host !== null })
            compare(panel.host.objectName, "pictureHost")
            verify(!panel.host.hasPicture)  // nothing decoded yet: the §11.8 sentence shows
            compare(panel.host.zoom, 0)     // fit
        }
    }
}
