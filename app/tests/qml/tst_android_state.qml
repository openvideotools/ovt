// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-A.455 (chapter 11 §11.5.7), desktop half of acceptance test 1: the Android root item publishes its state as JSON
// after every change (uiStateSaved, which MainActivity keeps for onSaveInstanceState), publishes nothing before
// MainActivity has handed it the saved state (restoredState), and a new root given that state shows the same frame,
// selection, file and layout. The test engine shares its singletons between the two roots, so they are reset in
// between, as a recreated Activity's new engine starts from defaults. The emulator half ("Don't keep activities") is
// StateRestoreTest.kt and the record docs/acceptance/wi/WI-A.455.md.

import QtQuick
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "AndroidState"
    when: windowShown

    Window {
        id: host

        width: 1280
        height: 800
        visible: true
    }

    Component {
        id: shellComponent

        AndroidShell {
            width: 1280
            height: 800
        }
    }

    SignalSpy {
        id: savedSpy

        signalName: "uiStateSaved"
    }

    function resetModels() {
        Project.closeAll()
        Selection.clearSelection("test")
        DockModel.resetLayout()
    }

    function init() {
        resetModels()
        savedSpy.clear()
    }

    function cleanup() {
        resetModels()
    }

    function createShell() {
        const shell = createTemporaryObject(shellComponent, host.contentItem)
        verify(shell !== null)
        savedSpy.target = shell
        savedSpy.clear()
        return shell
    }

    function lastSaved() {
        verify(savedSpy.count > 0)
        return JSON.parse(savedSpy.signalArguments[savedSpy.count - 1][0])
    }

    function test_nothingPublishedBeforeTheSavedStateArrives() {
        const shell = createShell()
        Selection.selectFrame(3, 3, "test")
        wait(50)
        compare(savedSpy.count, 0)
        shell.restoredState = "{}"
        tryVerify(function () { return savedSpy.count > 0 })
        compare(lastSaved().selection.frame[0], 3)
    }

    function test_stateSurvivesANewRoot() {
        const first = createShell()
        first.restoredState = "{}"
        Project.openFile("/nonexistent/wi-a455/clip.h264")
        compare(Project.fileCount, 1)
        Selection.selectFrame(42, 40, "test")
        Selection.selectBytes(1000, 24, "test")
        verify(DockModel.applyPreset("Compare"))
        tryVerify(function () { return savedSpy.count > 0 && lastSaved().selection.frame[0] === 42 })
        tryVerify(function () { return lastSaved().layout.preset === "Compare" })
        const saved = savedSpy.signalArguments[savedSpy.count - 1][0]
        const doc = JSON.parse(saved)
        compare(doc.version, 1)
        compare(doc.project.files.length, 1)
        verify(doc.view !== undefined, "the Picture panel's ViewState is saved")
        verify(saved.length <= 100 * 1024)
        first.destroy()
        wait(0)

        // What the recreated Activity's engine starts with.
        resetModels()
        compare(Project.fileCount, 0)
        compare(Selection.frameDecodeIndex, -1)

        const second = createShell()
        const restoredAt = Date.now()
        second.restoredState = saved
        compare(Project.fileCount, 1)
        compare(Selection.frameDecodeIndex, 42)
        compare(Selection.frameDisplayIndex, 40)
        compare(Selection.byteOffset, 1000)
        compare(Selection.byteLength, 24)
        compare(DockModel.layoutName, "Compare")
        verify(Date.now() - restoredAt < 2000)
        // The restored state is published again for the next save.
        tryVerify(function () { return savedSpy.count > 0 && lastSaved().selection.frame[0] === 42 })
    }

    function test_badSavedStateIsIgnored() {
        const shell = createShell()
        Selection.selectFrame(5, 5, "test")
        shell.restoredState = "{\"version\":99}"
        compare(Selection.frameDecodeIndex, 5)
        tryVerify(function () { return savedSpy.count > 0 })
    }
}
