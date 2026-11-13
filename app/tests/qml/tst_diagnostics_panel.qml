// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Diagnostics / Log panel and the Jobs popover (WI-1.453): the Errors tab shows a diagnostic and its count badge,
// the Log tab is reachable, and the popover opens from the toolbar button and lists "No jobs" when idle.

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "DiagnosticsPanel"
    when: windowShown

    Component {
        id: panelComponent

        DiagnosticsPanel {
            width: 600
            height: 240
        }
    }

    Component {
        id: mainComponent

        Main {
            visible: true
        }
    }

    function cleanup() {
        DiagnosticsModel.clear()
    }

    function test_errorsTabShowsDiagnosticAndBadge() {
        const panel = createTemporaryObject(panelComponent, testCase)
        verify(panel !== null)
        const errorsTab = findChild(panel, "errorsTab")
        compare(errorsTab.text, "Errors")
        DiagnosticsModel.addGui(2, "GUI-PROJ-0003", "file not found: clip.mp4")
        compare(DiagnosticsModel.errorCount, 1)
        compare(errorsTab.text, "Errors (1)")
        compare(findChild(panel, "errorsList").count, 1)
        // The text filter narrows the list.
        findChild(panel, "diagnosticsText").text = "nothing matches"
        compare(findChild(panel, "errorsList").count, 0)
    }

    function test_logTab() {
        const panel = createTemporaryObject(panelComponent, testCase)
        findChild(panel, "logTab").clicked()
        verify(findChild(panel, "logList") !== null)
    }

    function test_jobsPopoverAndBadge() {
        const window = createTemporaryObject(mainComponent, null)
        verify(window !== null)
        const popover = findChild(window, "jobsPopover")
        verify(popover !== null)
        verify(!popover.opened)
        findChild(window, "jobsButton").clicked()
        tryVerify(function () { return popover.opened })
        compare(JobsModel.activeCount, 0)
        verify(!findChild(window, "statusJobs").visible)
        DiagnosticsModel.addGui(2, "GUI-SET-0001", "boom")
        compare(findChild(window, "statusDiagnostics").text, "1 errors")
    }

    function test_saveLogWritesTheFile() {
        const panel = createTemporaryObject(panelComponent, testCase)
        findChild(panel, "logTab").clicked()
        findChild(panel, "saveLog").clicked()
        const popup = findChild(panel, "saveLogPopup")
        tryVerify(function () { return popup.opened })
        const dir = StandardPaths.writableLocation(StandardPaths.TempLocation).toString()
        const path = decodeURIComponent(dir.replace(/^file:\/\//, "")) + "/ovt-panel-log-test.txt"
        findChild(panel, "savePath").text = path
        findChild(panel, "saveConfirm").clicked()
        compare(findChild(panel, "saveResult").text, "Saved " + path)
        // An unwritable path reports the failure and writes nothing.
        findChild(panel, "savePath").text = "/nonexistent-ovt-dir/log.txt"
        findChild(panel, "saveConfirm").clicked()
        compare(findChild(panel, "saveResult").text, "Could not write /nonexistent-ovt-dir/log.txt")
        popup.close()
    }

    function test_diagnosticsIsADockPanelToggledByTheBadge() {
        const window = createTemporaryObject(mainComponent, null)
        verify(window !== null)
        const wasOpen = DockModel.openPanels.indexOf("diagnostics") >= 0
        const badge = findChild(window, "statusDiagnostics")
        badge.clicked()
        compare(DockModel.openPanels.indexOf("diagnostics") >= 0, !wasOpen)
        if (!wasOpen)
            tryVerify(function () { return findChild(window, "diagnosticsPanel") !== null })
        badge.clicked()
        compare(DockModel.openPanels.indexOf("diagnostics") >= 0, wasOpen)
        // The panel is created by the dock workspace, not as the placeholder.
        DockModel.openPanel("diagnostics")
        tryVerify(function () { return findChild(window, "diagnosticsPanel") !== null })
        if (!wasOpen)
            DockModel.closePanel("diagnostics")
    }
}
