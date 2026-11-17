# WI-A.451 acceptance record

Machines: desktop half in a cloud container (Linux x86_64, Ubuntu 24.04, Qt 6.4.2 from apt); emulator half on the Mac Mini.
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
./gradlew -p android :app:assembleFossDebug :app:installFossDebug   # emulator running
python3 -m pytest app/tests/scripts/test_android_breakpoints.py -v
```

The script sets the window size with `adb shell wm size` at the device density (so `Configuration.screenWidthDp` is
1300, 900, 700), starts the app and waits for the `qml` log lines `OVT: layout three|two|vertical widthDp=<n>
frame=<decode index>` that `CompactLayout.qml` writes on every class change. It asserts the class sequence, that the
app read the width it was given, and that `frame` did not change. It skips without `adb`/an attached emulator.
Mac Mini runs (emulator `ovt-api36-arm64`, 1080x2400, 420 dpi):

- 7c4a50d, 36cbdfc, 69e862e: failed. Besides two test-script bugs (log level, a duplicate launch line) and a
  QML item width in device pixels (device pixel ratio 1, so an item width is no longer used on Android), the
  resize never reached the layout after start-up. The cause was the emulator, not the app: started with
  `-gpu auto` on the headless Mini, it picked SwiftShader, and under CI load Qt's GUI thread blocked in
  `QWindow::setVisible` waiting for the render thread (stuck in `glGetProgramBinary`), so neither timers nor
  posted calls ran. With `-gpu host` the 69e862e poll passes too.
- 07d68cc (`-gpu host`, Apple M4 host GLES): **PASS**. The width is pushed, not polled:
  `MainActivity.onConfigurationChanged` sets the QML root's `configurationWidthDp` (`QtQuickView.setProperty`) and
  `AndroidShell.qml` calls `Platform.refresh(widthDp)`. Log of the 2026-11-17 run, kept in the project files as
  `wa451-pr707-07d68cc-mini-20261117T022943Z.txt`:

```console
$ python3 -m pytest app/tests/scripts/test_android_breakpoints.py   # no media
============================== 1 passed in 6.98s ===============================
I default : OVT: Platform read widthDp=1300 ok=1 appState=2
D qml     : OVT: layout three widthDp=1300 frame=-1
I OVT     : OVT: configuration screenWidthDp=900 shellReady=true
D qml     : OVT: configuration widthDp=900
D qml     : OVT: layout two widthDp=900 frame=-1
I OVT     : OVT: configuration screenWidthDp=700 shellReady=true
D qml     : OVT: configuration widthDp=700
D qml     : OVT: layout vertical widthDp=700 frame=-1
```

  Manual resizes 1300 → 900 → 700 → 1300 dp and 1300 → 900 → 700 → 600 → 500 dp (4 s per step) followed every
  step (three, two, vertical, three; vertical at 600, single at 500; 700 → 1300 grows to three) with no Activity
  recreation. A `layout <old class>
  widthDp=<new>` line can precede the new class in the same millisecond; the test keeps the last line of each run.
  Desktop coverage: Catch2 "Platform: a pushed width change switches the layout class" and
  `tst_android_shell.qml` `test_configurationWidthDrivesTheLayout`.
- Frame half **blocked**: a VIEW intent opens the fd natively (`OVT: opened fd size=...`) but the open file never
  reaches the QML `Project`/`Selection` (the debug app still shows the no-file page and `frame` stays -1). That is
  the gap between `OvtSession` and the Project model (WI-A.452 / WI-1.509b), not this item, so "frame is kept" is
  proved on the desktop only (`tst_breakpoints.qml`, frame 42) until that lands.
