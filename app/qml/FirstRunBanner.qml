// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// First-run banners (chapter 11 §11.8, WI-1.464): the privacy notice on the first start of a profile, and on Android
// the note about file access and the cache location. Each is dismissible and stays dismissed: the choice is stored in
// settings.json under "privacy" (`first_run_banner_dismissed`, `android_banner_dismissed`), so a banner is shown
// until the user dismisses it once. Platform.isFirstRun (a fresh profile) is only used by tests.

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
    readonly property bool active: forceShow || !dismissed("first_run_banner_dismissed")

    // Reads the stored choice; the revision read makes bindings re-evaluate when a value is written.
    function dismissed(key) {
        return Settings.revision >= 0 && Settings.value("privacy", key, false) === true
    }

    component Banner: Pane {
        id: banner

        property alias text: label.text
        property alias dismissName: dismiss.objectName
        // The settings.json key under "privacy" that remembers the dismissal.
        required property string settingKey

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
                onClicked: Settings.setValue("privacy", banner.settingKey, true)
            }
        }
    }

    Banner {
        objectName: "privacyBanner"
        dismissName: "privacyBannerDismiss"
        settingKey: "first_run_banner_dismissed"
        visible: banners.active && !banners.dismissed(settingKey)
        text: qsTr("%1 sends no data anywhere: no telemetry, no crash reporter.").arg(Qt.application.displayName)
    }

    Banner {
        objectName: "androidBanner"
        dismissName: "androidBannerDismiss"
        settingKey: "android_banner_dismissed"
        visible: Platform.isAndroid && !banners.dismissed(settingKey)
        text: qsTr("Files are opened through the Android file picker (Storage Access Framework), and the analysis cache lives in the app's private storage; clear it from Preferences or Android's app settings.")
    }
}
