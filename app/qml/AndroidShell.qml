// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The root item that MainActivity's QtQuickView loads on Android (chapter 13 §13.4.3; WI-0.050, WI-A.450).
// QtQuickView shows an Item inside the Activity and cannot take a Window, so the desktop Main.qml (an
// ApplicationWindow) is not loaded here. SPEC-ASSUMPTION(#356): WI-A.450 brings the desktop shell into the Activity
// by hosting the same pieces Main.qml hosts (OvtMenuBar, MainToolBar, AppShell, StatusBar) in a Page, an Item that
// carries the window palette and font as ApplicationWindow does on the desktop. The Welcome page (WI-1.464) is
// therefore what a fresh launch shows (WI-A.450 acceptance test 1). The breakpoint layouts of §11.5.2 are WI-A.451.
// Qt 6.8 does not expose the Qt Quick accessibility tree through QtQuickView (issue #547), so MainActivity reads
// `productName` once the root is ready and puts it on the Qt view's own accessibility node, which UI Automator finds
// (WI-0.050 acceptance test 2).
//
// WI-0.052 painted only a header band so that the HDR test pattern of HdrSurfaceView (below QtQuickView, chapter 13
// §13.4.4 item 1) showed through; the full shell is opaque and covers that surface. WI-A.400 cuts the Picture
// panel's hole and replaces MainActivity.SHELL_HEADER_FRACTION with the panel's geometry.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Page {
    id: root

    // The shell, for tests.
    readonly property alias shellItem: shell
    // The product name of the application, read by MainActivity through QtQuickView.getProperty() (issue #547).
    readonly property string productName: Qt.application.displayName
    // The Activity's Configuration.screenWidthDp, set by MainActivity.onConfigurationChanged through
    // QtQuickView.setProperty() (WI-A.451, §11.5.2): the window width the breakpoint layouts follow after start-up.
    // 0 until the first change; Platform reads the start-up width itself.
    property int configurationWidthDp: 0
    onConfigurationWidthDpChanged: {
        if (configurationWidthDp > 0) {
            console.log("OVT: configuration widthDp=" + configurationWidthDp)
            Platform.refresh(configurationWidthDp)
        }
    }

    // Main.qml's window palette, less placeholderText, which a Control's palette does not offer on Qt 6.4 (the
    // oldest Qt the desktop tests build with); the controls' own default applies there.
    palette {
        window: OvtStyle.window
        windowText: OvtStyle.text
        base: OvtStyle.panel
        alternateBase: OvtStyle.window
        text: OvtStyle.text
        button: OvtStyle.panel
        buttonText: OvtStyle.text
        highlight: OvtStyle.highlight
        highlightedText: OvtStyle.highlightedText
        mid: OvtStyle.border
        dark: OvtStyle.border
        light: OvtStyle.panel
        toolTipBase: OvtStyle.panel
        toolTipText: OvtStyle.text
    }
    font.pixelSize: OvtStyle.fontSize

    background: Rectangle {
        color: OvtStyle.window
    }

    header: ColumnLayout {
        spacing: 0

        OvtMenuBar {
            objectName: "menuBar"
            Layout.fillWidth: true
            actions: shell.actions
        }
        MainToolBar {
            objectName: "toolBar"
            Layout.fillWidth: true
            onJobsClicked: shell.toggleJobs()
        }
    }

    AppShell {
        id: shell

        anchors.fill: parent
    }

    footer: StatusBar {}

    // As Main.qml does on the desktop: bookmarks follow the open project (WI-2.487).
    Component.onCompleted: Bookmarks.attach(Project, Selection)
}
