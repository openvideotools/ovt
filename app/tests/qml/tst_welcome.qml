// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.464 acceptance test 1: on a fresh profile the Welcome page and the privacy banner appear (both themes), plus
// the page's parts (recent files with pin and remove, the consent dialog with size and licence) and the empty states.

import QtQuick
import QtQuick.Controls
import QtTest
import OvtStudio

TestCase {
    id: testCase

    name: "Welcome"
    when: windowShown

    Component {
        id: mainComponent

        Main {
            visible: true
            title: "OVT Studio"
        }
    }

    Component {
        id: pageComponent

        WelcomePage {
            // A top-level item has no window of its own, so it is hidden; the tests need it shown.
            property Window host: Window {
                width: 800
                height: 600
                visible: true
            }
            parent: host.contentItem
            width: 800
            height: 600
            sampleEntries: [{"id": "syn_test", "url": "https://example.invalid/syn_test.zip", "sizeText": "1.7 MB",
                             "licence": "CC0 test licence", "licenceSpdx": "CC0-1.0"}]
        }
    }

    Component {
        id: emptyComponent

        EmptyState {}
    }

    function init() {
        Settings.setValue("privacy", "first_run_banner_dismissed", false)
        Project.closeAll()
        RecentFiles.clear()
    }

    function cleanup() {
        RecentFiles.clear()
    }

    function visibleOnScreen(item) {
        return item !== null && item.visible && item.width > 0 && item.height > 0
    }

    // Waits for the first layout pass (a new window starts with zero-sized items).
    function verifyShown(window, name) {
        tryVerify(function () { return visibleOnScreen(findChild(window, name)) })
    }

    // findChild does not see items made by a Repeater: walk the visual children.
    function findByName(item, objectName) {
        if (item.objectName === objectName)
            return item
        const kids = Array.from(item.children)
        if (item.contentItem)  // a ScrollView/Flickable keeps its content in the content item
            kids.push(item.contentItem)
        for (const kid of kids) {
            const found = findByName(kid, objectName)
            if (found)
                return found
        }
        return null
    }

    function test_freshProfileShowsWelcomeAndPrivacyBanner() {
        verify(Platform.isFirstRun)
        for (const theme of ["light", "dark"]) {
            Settings.theme = theme
            const window = createTemporaryObject(mainComponent, null)
            verify(window !== null)
                        verifyShown(window, "welcomePage")
            verifyShown(window, "privacyBanner")
            verify(findChild(window, "firstRunBanners").active)
            verify(!findChild(window, "androidBanner").visible || Platform.isAndroid)
            compare(findChild(window, "welcomeDropZone").visible, true)
            window.destroy()
        }
        Settings.theme = "system"
    }

    function test_privacyBannerCanBeDismissed() {
        const window = createTemporaryObject(mainComponent, null)
        const banner = findChild(window, "privacyBanner")
        verify(banner.visible)

        verify(banner.Accessible.name.indexOf("no telemetry, no crash reporter") >= 0)
        findChild(window, "privacyBannerDismiss").clicked()
        verify(!banner.visible)
        // The choice is stored, so a new window (a later launch) does not show the banner again.
        compare(Settings.value("privacy", "first_run_banner_dismissed", false), true)
        const again = createTemporaryObject(mainComponent, null)
        verify(!findChild(again, "privacyBanner").visible)
    }

    function test_welcomeGoesAwayWhenAFileOpens() {
        const window = createTemporaryObject(mainComponent, null)
        verifyShown(window, "welcomePage")
        Project.openFile(Qt.resolvedUrl("tst_welcome.qml"))
        compare(Project.fileCount, 1)
        tryVerify(function () { return !findChild(window, "welcomePage").visible })
    }

    function test_recentFilesPinAndRemove() {
        const page = createTemporaryObject(pageComponent, testCase)
        verify(findByName(page, "welcomeNoRecent").visible)
        RecentFiles.add("/data/a.264", "h264")
        RecentFiles.add("/data/b.265", "hevc")
        compare(RecentFiles.entries.length, 2)
        verify(!findByName(page, "welcomeNoRecent").visible)
        const first = findByName(page, "recentOpen_0")
        verify(first !== null)
        findByName(page, "recentPin_1").clicked()
        compare(RecentFiles.entries[0].pinned, true)
        findByName(page, "recentRemove_0").clicked()
        compare(RecentFiles.entries.length, 1)
    }

    function test_sampleStreamNeedsConsent() {
        const page = createTemporaryObject(pageComponent, testCase)
        const consented = []
        page.sampleConsented.connect(id => consented.push(id))
        const dialog = findChild(Overlay.overlay.parent, "sampleStreamsDialog")
        verify(dialog !== null)
        findByName(page, "sample_0").clicked()
        tryVerify(function () { return dialog.opened })
        compare(findChild(dialog.contentItem, "sampleSize").text, "Size: 1.7 MB")
        verify(findChild(dialog.contentItem, "sampleLicence").text.indexOf("CC0 test licence") >= 0)
        compare(consented.length, 0)  // opening the dialog downloads nothing
        dialog.reject()
        compare(consented.length, 0)
        findByName(page, "sample_0").clicked()
        tryVerify(function () { return dialog.opened })
        dialog.accept()
        compare(consented, ["syn_test"])
    }

    function test_emptyStateSentences() {
        const state = createTemporaryObject(emptyComponent, testCase)
        compare(state.textFor("timeline"), "Open a file to see its frames.")
        compare(state.textFor("syntax"), "Select a unit in the Unit list or a frame.")
        compare(state.textFor("bookmarks"), "Press B to bookmark the current frame.")
        compare(state.textFor("display_caps"), "")
    }
}
