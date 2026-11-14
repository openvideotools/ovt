# WI-A.450 acceptance record

Machine: cloud container, Linux x86_64 (Ubuntu 24.04), Qt 6.4.2 from apt, no Android SDK/NDK, no emulator, no
Kotlin compiler (Google's Maven and the SDK downloads are blocked by the container's proxy).
Date: 2026-11-14. Spec version: 1.0-rc24.

**Status: the emulator runs have not been done.** The QML half of test 1 runs on the desktop and passes; the APK
and the instrumentation tests could not be built here. The emulator runs are owed before this work item is Done;
the commands are below so that the Mac Mini session or a CI emulator job (WI-0.034, WI-1.577) can fill in the
output.

## Test 1: the APK launches on the x86_64 API 36 emulator and the Welcome page shows

What changed: `AndroidShell.qml`, the Item that `QtQuickView` loads, now hosts the desktop shell (`AppShell.qml`
with `OvtMenuBar`, `MainToolBar` and `StatusBar`, the same pieces `Main.qml` hosts) instead of the WI-0.050
placeholder text, so the Welcome page is what a fresh launch shows.

Desktop half, automated: `tst_android_shell.qml` (`ctest --preset pr -R ovt_tests_app_qml`) instantiates
`AndroidShell` in a plain window and checks that it is an `Item`, that the Welcome page is on screen, that the
8 menus, the toolbar and the status bar are there, and that the Jobs popover opens below the toolbar. Run here
with `QT_QPA_PLATFORM=offscreen build/linux-x64-debug/app/tests/ovt_tests_app_qml`:

```console
PASS   : tst_main::AndroidShell::test_jobsPopover()
PASS   : tst_main::AndroidShell::test_menusToolBarAndStatusBar()
PASS   : tst_main::AndroidShell::test_welcomePageShows()
Totals: 65 passed, 0 failed, 0 skipped, 0 blacklisted
```

(65 is the whole QML suite, so `tst_main`, `tst_welcome`, `tst_open` and `tst_jobs_popover` still pass on the
refactored `Main.qml`.)

Emulator half, owed:

```console
$ ./gradlew -p android :app:assembleFossDebug :app:installFossDebug   # emulator x86_64 API 36 running
$ adb shell am start -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity
$ adb shell uiautomator dump /sdcard/ui.xml && adb pull /sdcard/ui.xml && grep -o 'Welcome' ui.xml | head -1
Welcome                                                # the Welcome page's Accessible.name
```

Output: NOT YET RECORDED (plus a screenshot, `adb exec-out screencap -p > welcome.png`).

## Test 2: an `ACTION_VIEW` intent opens the file in the running Activity without recreation

Automated: `SessionTest.viewIntentOpensFileInRunningActivity`
(`android/app/src/androidTest/java/org/openvideotools/studio/SessionTest.kt`). With the Activity running, a VIEW
intent arrives; the test waits until `OvtSession` has opened the file and checks that the Activity instance is the
same one and still resumed (`launchMode="singleTask"` delivers it to `onNewIntent`). Manual cross-check with a
real provider:

```console
$ adb push mini.mp4 /sdcard/Download/
$ adb shell am start -a android.intent.action.VIEW -t video/mp4 \
    -d content://com.android.externalstorage.documents/document/primary%3ADownload%2Fmini.mp4
$ adb logcat -d -s OVT | grep 'opened fd size='
```

Output: NOT YET RECORDED (`./gradlew -p android :app:connectedFossDebugAndroidTest`).

## Test 3: with "Don't keep activities", recreation keeps the open handles

Automated: `SessionTest.recreationKeepsTheOpenHandles` (`ActivityScenario.recreate()`, which is what "Don't keep
activities" does on every navigation away) checks that `OvtSession` holds the same native session handle, that the
handle is still live (`ovt_file_size` through it), that `OvtSession.openCount` did not change (nothing was opened,
so nothing was indexed again) and that the new Activity re-bound. `SessionTest.finishingClosesTheSession` checks
that the handles are released when the Activity finishes. SPEC-ASSUMPTION(#444): there is no `ovt_Stream` or
`source_hash` in the Android session yet, so the same live handle and the unchanged open count stand in for "same
`source_hash`, no re-index".

Manual cross-check:

```sh
adb shell settings put global always_finish_activities 1
adb shell am start -a android.intent.action.VIEW -t video/mp4 -d <content URI as in test 2>
adb shell input keyevent KEYCODE_HOME && adb shell am start -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity
adb logcat -d -s OVT | grep -c 'opened fd size='   # prints 1: the file was opened once
adb shell settings put global always_finish_activities 0
```

Output: NOT YET RECORDED.
