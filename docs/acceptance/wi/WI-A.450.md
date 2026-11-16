# WI-A.450 acceptance record

Machine: cloud container, Linux x86_64 (Ubuntu 24.04), Qt 6.4.2 from apt, no Android SDK/NDK, no emulator, no
Kotlin compiler (Google's Maven and the SDK downloads are blocked by the container's proxy).
Date: 2026-11-14 (desktop), 2026-11-16 (emulator, and a re-run at 77a63a1 below). Spec version: 1.0-rc24.

**Status: all three tests pass.** The QML half of test 1 runs on the desktop (below). The emulator runs were done on
the Mac Mini (Mac16,10, Apple M4, macOS 15.8.2), AVD `ovt-api36-arm64` (android-36 google_apis arm64-v8a,
headless; the Mini has no x86_64 system image, so the arm64-v8a APK was used), on 2026-11-16: the instrumentation
tests and test 1 at commit 565b1a8, the manual cross-checks of tests 2 and 3 at 301b400. Build:
`./gradlew -p android :app:QtBuildTask --rerun :app:assembleFossDebug :app:assembleFossDebugAndroidTest`.

Finishing the Activity after the Qt UI loaded deadlocked in Qt 6.8's `QtNative.quitQt()` until the fix of issue #614
(PR #615, merged into this branch): the instrumentation tests below depend on it.

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
$ adb logcat -d | grep -o 'OVT: Welcome page shown' | head -1
OVT: Welcome page shown                                # WelcomePage.qml logs when its shown copy becomes visible
```

The probe is a log line, not `uiautomator dump`: Qt 6.8's `QtQuickView` registers no accessibility interface
(issue #547), so the whole QML scene is one `TextureView` without child nodes and uiautomator cannot see the
Welcome page's `Accessible.name`. The screenshot is the visual check.

Output (565b1a8; the line comes from the `qml` log tag; screenshot in the project files,
`wa450-teardown-final-565b1a8-C.png`, not committed):

```console
11-16 00:03:51.650  4327  4352 I qml     : OVT: Welcome page shown
11-16 00:03:52.510  4327  4327 I OVT     : OVT: QML root ready, title 'OVT Studio'
```

PASS.

## Test 2: an `ACTION_VIEW` intent opens the file in the running Activity without recreation

Automated: `SessionTest.viewIntentOpensFileInRunningActivity`
(`android/app/src/androidTest/java/org/openvideotools/studio/SessionTest.kt`). With the Activity running, a VIEW
intent arrives; the test waits until `OvtSession` has opened the file and checks that the Activity instance is the
same one and still resumed (`launchMode="singleTask"` delivers it to `onNewIntent`). Manual cross-check with a
real provider:

```console
$ adb push mini.mp4 /sdcard/Download/
$ adb shell content query --uri content://media/external/video/media --projection _id:_display_name  # mini.mp4's id
$ adb shell am start -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity \
    -a android.intent.action.VIEW -t video/mp4 --grant-read-uri-permission -d content://media/external/video/media/<id>
$ adb logcat -d -s OVT | grep 'opened fd size='
```

A MediaStore URI with `-n` and `--grant-read-uri-permission`: without `-n` another app that handles `video/mp4` can
take the intent (a chooser opens), and `adb shell` cannot grant read access to an ExternalStorageProvider document URI
(Permission Denial).

Output. Instrumented, 565b1a8, each case alone and then the whole class in one process
(`adb shell am instrument -w -r -e class org.openvideotools.studio.SessionTest
org.openvideotools.studio.debug.test/androidx.test.runner.AndroidJUnitRunner`):

```console
viewIntentOpensFileInRunningActivity: OK (1 test)
SessionTest (whole class): OK (3 tests)
```

Manual cross-check, 301b400 (MediaStore id of `mini.mp4`: 16):

```console
Warning: Activity not started, intent has been delivered to currently running top-most instance.
11-15 22:38:36.787  4448  4523 I OVT     : opened fd size=475988
```

PASS.

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
adb shell am start -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity -a android.intent.action.VIEW -t video/mp4 --grant-read-uri-permission -d <MediaStore URI as in test 2>
adb shell input keyevent KEYCODE_HOME && adb shell am start -n org.openvideotools.studio.debug/org.openvideotools.studio.MainActivity
adb logcat -d -s OVT | grep -c 'opened fd size='   # prints 1: the file was opened once
adb shell settings put global always_finish_activities 0
```

Output. Instrumented, 565b1a8:

```console
recreationKeepsTheOpenHandles: OK (1 test)
finishingClosesTheSession: OK (1 test)
```

Manual cross-check, 301b400:

```console
Warning: Activity not started, its current task has been brought to the front
grep -c 'opened fd size=': 1
```

PASS. Also checked at 565b1a8: Back from the loaded UI destroys the Activity without an ANR, and a warm relaunch
in the same process (same pid) shows the Welcome page again (`wa450-teardown-final-565b1a8-D.png`).

## Re-run after merging main (77a63a1)

The merge of main (#615, WI-1.405) changed `MainActivity.onDestroy` and `AppShell.qml` after the runs above, so
the emulator checks were repeated at 77a63a1 with no patch (Mac Mini, same AVD, 2026-11-16; project files
`wa450-final-77a63a1-results.txt`):

```console
recreationKeepsTheOpenHandles: OK (1 test)
finishingClosesTheSession: OK (1 test)
viewIntentOpensFileInRunningActivity: OK (1 test)
SessionTest, whole class in one process: OK (3 tests)
relaunch: OVT: Welcome page shown
```

In that run, the Back checks failed once. Back was sent the moment `OVT: QML root ready` appeared on the first launch
after a fresh install, and the Activity was not destroyed, so the warm relaunch had nothing to do. This did not
reproduce in 26 later trials at 77a63a1 (project files `wa450-back-bisect-results.txt` and
`wa450-back-race-results.txt`):
- The first 4 trials sent Back 3 s after root-ready.
- The other 22 used a timing sweep: fresh install, cold and warm starts, with Back sent 0, 0.5, 1 and 2 s after
  root-ready, three times at 0 s on a fresh install.

A diagnostic build logged every `KEYCODE_BACK` the Activity received. Each one arrived with window focus while the
Activity was RESUMED, and `onDestroy` followed. The one failure is recorded as an emulator artefact, not a lost Back.
From now on, the Back checks send Back only once `dumpsys window` reports `mCurrentFocus` on `MainActivity`, never
after a fixed sleep.
