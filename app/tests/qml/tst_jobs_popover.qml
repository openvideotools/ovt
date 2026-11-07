// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Jobs popover and the status-bar job read-out (WI-1.453): the popover opens from the toolbar button and lists
// "No jobs" when idle; the progress bar and text in the status bar are hidden when no job runs.

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "JobsPopover"
    when: windowShown

    Component {
        id: mainComponent

        Main {
            visible: true
        }
    }

    function test_popoverOpensFromToolbar() {
        const window = createTemporaryObject(mainComponent, null)
        verify(window !== null)
        const popover = findChild(window, "jobsPopover")
        verify(popover !== null)
        verify(!popover.opened)
        findChild(window, "jobsButton").clicked()
        tryVerify(function () { return popover.opened })
        compare(JobsModel.activeCount, 0)
        verify(!findChild(window, "cancelAll").enabled)
    }

    function test_statusBarIdle() {
        const window = createTemporaryObject(mainComponent, null)
        verify(!findChild(window, "statusJobs").visible)
        verify(!findChild(window, "statusJobText").visible)
    }
}
