# WI-A.452 acceptance record

Machine: cloud container, Linux x86_64, no Android SDK/NDK and no emulator (Google's Maven and the SDK downloads are
blocked by the container's proxy). Date: 2026-11-17. Spec version: 1.0-rc41.

**Status: PASS.** `SafFlowsTest` 9 of 9 at c642e04, and test 1 by hand at 89fedca. They run on the Mac Mini's API 36
emulator (AVD `ovt-api36-arm64`, arm64-v8a, `-gpu host`) until the Android instrumentation job of
WI-1.577 exists.

Run at 89fedca (2026-11-17, Mini): both assemble tasks pass. `SafFlowsTest`: `exportCreatesAndWritesTheDocument`,
`cancelledCopyLeavesNoFile`, `fstatDecidesSeekability` and `recentEntryOfAGoneDocumentIsRemoved` PASS. Then the app
crashed in Qt during `documentOpensShowsInRecentListAndReopens`: an NPE in `QtDisplayManager.setApplicationDisplayMetrics`
(Qt 6.8.3), whose start-up action outlived an Activity finished right after launch and read its null insets. Fix:
`MainActivity.finish()` waits for `OvtQtResume.whenStartupActionsRan` (at most 2 s). At 11cf4cb that worked, but a late
QML status change of the destroyed Activity's view then hit its freed QQuickView (SIGSEGV); it is now dropped.

Run at c642e04 (2026-11-17 06:50 UTC, Mini): both assemble tasks pass; `SafFlowsTest` finished 9 tests, all PASS, no
crash or FATAL in logcat ("finish deferred until Qt has started", then "Qt started, finishing now" 0.5 s later).

What was checked here: every changed Kotlin file, `SafFlowsTest.kt` and the debug-only `TestDocumentsProvider.kt`
compile with `kotlinc` 2.0.21 `-Werror` against `android-all` 16 (Robolectric's API 36 jar) and stubs of the AndroidX
and Qt classes. ktlint 1.5.0 (pre-commit) passes. CI's `assembleFossDebug` compiles the main and debug sources.

Build and run:

```console
$ ./gradlew -p android :app:assembleFossDebug :app:assembleFossDebugAndroidTest
$ ./gradlew -p android :app:connectedFossDebugAndroidTest \
    -Pandroid.testInstrumentationRunnerArguments.class=org.openvideotools.studio.SafFlowsTest
```

## Test 1: a document URI opens through `ovt_file_open_fd`, shows in the recent list, and reopens after a restart

Automated: `SafFlowsTest.documentOpensShowsInRecentListAndReopens` (open through `FilePicker.open(uri, persist =
true)`, which is what the picker's result callback calls; the recent entry's display name; the reopen from the
stored list in a new Activity) and `SafFlowsTest.recentEntryOfAGoneDocumentIsRemoved`. Supporting:
`recentListOrderAndCapacity`, `grantEvictionIsOldestFirstAndKeepsRecentsLongest`.

The test provider belongs to the app's own UID, so it offers no persistable grant. The persisted grant and a real
process restart are checked by hand with the system picker:

```console
$ adb shell am start -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity
# tap "Open file…" in the debug overlay, pick a video in Downloads
$ adb shell dumpsys activity permissions | grep -A2 org.openvideotools.studio.debug   # persisted read grant listed
$ adb shell am force-stop org.openvideotools.studio.debug
$ adb shell am start -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity
# the overlay lists the file under "Recent files"; tap it: no picker appears and the overlay shows its info JSON
```

Output (89fedca, Mini, 2026-11-17; real DocumentsUI, `mini.mp4` in Downloads): PASS.

```console
# picked mini.mp4: logcat "OVT: opened fd size=475988"; overlay "Recent files / mini.mp4 · isobmff"
$ adb shell am force-stop org.openvideotools.studio.debug          # pidof empty afterwards
$ adb shell am start -W -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity   # TotalTime 1774 ms
# overlay still lists "mini.mp4 · isobmff" with "no file" open; tapped it: "OVT: opened fd size=475988", no
# SecurityException, info JSON {"name":"mini.mp4","size":475988,"format":"isobmff","track_count":0,"tracks":[]}
```

Screenshots in the project files (`wa452-at1-opened.png`, `wa452-at1-relaunch.png`, `wa452-at1-reopened.png`), not
committed. `track_count` 0 comes from WI-0.051's info JSON and the core's ISOBMFF track list, not from this WI.

## Test 2: a non-seekable provider raises `GUI-AND-0001` with the copy action, and the copy opens

Automated: `SafFlowsTest.nonSeekableDocumentRaisesGuiAnd0001AndOpensAfterCopy` (a `pipe-` document of the test
provider is a pipe; the `GUI-AND-0001` dialog appears, nothing reaches the core, "Copy to app storage" streams the
document to `getExternalFilesDir(null)/copies/` (SPEC-ASSUMPTION(#722)) and the copy opens and enters the recent
list). Supporting: `fstatDecidesSeekability` (PASS at 89fedca), `copyNamesAreSafeAndUnique`,
`cancelledCopyLeavesNoFile` (PASS at 89fedca).

Output: PASS at c642e04 (`nonSeekableDocumentRaisesGuiAnd0001AndOpensAfterCopy` and its supporting tests).

## Export (step 3, no acceptance line)

`SafFlowsTest.exportCreatesAndWritesTheDocument`: the `ACTION_CREATE_DOCUMENT` intent carries the MIME type and the
suggested name, and the writer's bytes land in the created document. By hand: open a file, tap "Export info…",
save to Downloads, and check the JSON there.
