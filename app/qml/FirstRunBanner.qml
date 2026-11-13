// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// First-run banners (chapter 11 §11.8, WI-1.464): the privacy notice on the first start of a profile, and on Android
// the note about file access and the cache location. Dismissible; Platform.isFirstRun is true only for the process
// that started without a settings.json, so the banner is shown once per profile.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

ColumnLayout {
    id: banners

    objectName: "firstRunBanners"
    spacing: 0
    // For tests: the banners show regardless of Platform.isFirstRun.
    property bool forceShow: false
    readonly property bool active: forceShow || Platform.isFirstRun

    component Banner: Pane {
        id: banner

        property alias text: label.text
        property alias dismissName: dismiss.objectName

        Layout.fillWidth: true
        padding: OvtStyle.spacing
        background: Rectangle {
            color: OvtStyle.panel
            border.color: OvtStyle.border
        }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: label.text

        RowLayout {
            anchors.fill: parent
            spacing: OvtStyle.spacing * 2

            Label {
                id: label

                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: OvtStyle.text
            }
            Button {
                id: dismiss

                text: qsTr("Dismiss")
                onClicked: banner.visible = false
            }
        }
    }

    Banner {
        objectName: "privacyBanner"
        dismissName: "privacyBannerDismiss"
        visible: banners.active
        text: qsTr("%1 sends no data anywhere: no telemetry, no crash reporter.").arg(Qt.application.displayName)
    }

    Banner {
        objectName: "androidBanner"
        dismissName: "androidBannerDismiss"
        visible: banners.active && Platform.isAndroid
        text: qsTr("Files are opened through the Android file picker (Storage Access Framework), and the analysis cache lives in the app's private storage; clear it from Preferences or Android's app settings.")
    }
}
