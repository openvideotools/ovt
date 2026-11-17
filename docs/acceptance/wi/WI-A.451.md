# WI-A.451 acceptance record

Machine: cloud container, Linux x86_64 (Ubuntu 24.04), Qt 6.4.2 from apt, no Android SDK/NDK, no emulator.
Date: 2026-11-17. Spec version: see the PR body.

## Test 1: resizing 1300 → 900 → 700 dp switches three-pane → two-pane → vertical without losing `Selection.frame`

Desktop half, automated: `app/tests/qml/tst_breakpoints.qml` (`ctest --preset pr -R ovt_tests_app_qml`). `Platform.widthDp`
is writable, so the test stands in for the Android Configuration; the shell is created with `compactEnabled: true`.
It selects frame 42, sets the width to 1300, 900, 700, 500 and back, and checks the layout class, the orientation of
the dock's outer `SplitView`, the 60/40 split and 120 dp timeline of the two-pane layout, the picture / timeline /
tabs order and 96 dp strip of the vertical layout, the bottom navigation of the single pane, that
`Selection.frameDecodeIndex` stays 42 and that the Picture panel is the same item after the switches.

```console
$ QT_QPA_PLATFORM=offscreen build/linux-x64-debug/app/tests/ovt_tests_app_qml
PASS   : tst_main::Breakpoints::test_resizeSwitchesLayoutsKeepingSelection()
...
Totals: 99 passed, 0 failed, 0 skipped, 0 blacklisted
```

Emulator half, owed (the emulator CI of WI-0.035 / #487 is not merged):

```console
$ ./gradlew -p android :app:assembleFossDebug :app:installFossDebug   # emulator running
$ python3 -m pytest app/tests/scripts/test_android_breakpoints.py -v
```

The script sets the window size with `adb shell wm size` at the device density (so `Configuration.screenWidthDp` is
1300, 900, 700), starts the app and waits for the `qml` log lines `OVT: layout three|two|vertical widthDp=<n>
frame=<decode index>` that `CompactLayout.qml` writes on every class change. It asserts the class sequence, that the
app read the width it was given, and that `frame` did not change. It skips without `adb`/an attached emulator.
Run it on the Mac Mini AVD (`ovt-api36-arm64`) and paste the output here: **not yet run**.
