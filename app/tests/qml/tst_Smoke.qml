// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-0.568 smoke test of the Qt Quick Test target (chapter 14 §14.10.1): the runner starts offscreen, the OvtStudio
// module loads and a grab of an Item has the size of the Item. WI-1.576 adds the model tests next to it.

import QtQuick
import QtTest
import OvtStudio

TestCase {
    name: "Smoke"
    when: windowShown
    width: 200
    height: 100

    Rectangle {
        id: red

        anchors.fill: parent
        color: "#ff0000"
    }

    function test_platform_is_offscreen() {
        compare(Qt.platform.pluginName, "offscreen")
    }

    function test_style_singleton_loads() {
        verify(OvtStyle.fontSize > 0)
    }

    function test_grab_has_item_size() {
        const result = grabImage(red)
        compare(result.width, 200)
        compare(result.height, 100)
    }
}
