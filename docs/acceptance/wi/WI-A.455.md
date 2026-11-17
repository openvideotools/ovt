# WI-A.455 acceptance record

Machines: desktop half in a cloud container (Linux x86_64, Ubuntu 24.04, Qt 6.4.2 from apt, GCC 13); emulator half
on the Mac Mini (owed, see below). Date: 2026-11-17. Spec version: see the PR body.

## Test 1: with "Don't keep activities" on, backgrounding and resuming shows the same frame and selection within 2 s

### How the state travels

Each `QtQuickView` builds its own QML engine, so a recreated `MainActivity` starts with fresh `Selection`, `Project`,
`DockModel` and Picture panel. `OvtSession` (WI-A.450) keeps only the core handles. WI-A.455 adds:

- `app/src/platform/android_state.cpp` (`AndroidState`, QML element): the §11.5.7 keys as one JSON document of at most
  100 KB: project path and files (path or `content://` URI, role, track), `SelectionState`
  (`SelectionModel::Snapshot`), `ViewState` (`PictureController::ViewSnapshot`: fit, zoom, pan, plane mode, stage,
  show cropped), the layout (`DockModel` layout JSON, preset in `layout.preset`) and panel state (focused panel,
  drawer, single-pane tab). Over 100 KB it drops panel state, then layout.
- `AndroidShell.qml` emits `uiStateSaved(json)` after every change. It publishes nothing until `MainActivity` has set
  `restoredState` (the saved JSON, or `{}`), so a fresh engine's defaults never overwrite the state still to be restored.
- `MainActivity.kt` keeps the latest published JSON (a `QtQuickView` signal listener). `onSaveInstanceState` copies
  that string into the Bundle, and `onCreate` hands the Bundle's copy to the new root once it is READY. Saving
  never waits on the Qt thread.

### Desktop half, automated

`ctest --preset pr -R "ovt_tests_app_(models|qml)"`:

- `app/tests/models/test_android_state.cpp` (`[android]`): round trip into fresh models (frame, block, bytes, probe,
  rectangle, file re-numbering, active file, plane, stage, zoom and pan applied when the picture arrives, Compare
  layout, panel state); byte-identical capture twice (§0.7.5); a project that still has its files is kept; a fitted
  view refits; a preset-only layout; the 100 KB reduction; wrong version or non-JSON restores nothing; large byte
  offsets and "no block".
- `app/tests/qml/tst_android_state.qml`: nothing is published before `restoredState`; a second `AndroidShell` given
  the first one's saved JSON, after the singletons are reset to defaults, shows frame 42/40, the byte selection,
  the file and the Compare layout (measured within 2 s, and in practice in the same event-loop turn).

```console
$ QT_QPA_PLATFORM=offscreen build/linux-x64-debug/app/tests/ovt_tests_app_models "[android]"
All tests passed (68 assertions in 7 test cases)
$ QT_QPA_PLATFORM=offscreen build/linux-x64-debug/app/tests/ovt_tests_app_qml
PASS   : tst_main::AndroidState::test_badSavedStateIsIgnored()
PASS   : tst_main::AndroidState::test_nothingPublishedBeforeTheSavedStateArrives()
PASS   : tst_main::AndroidState::test_stateSurvivesANewRoot()
Totals: 106 passed, 0 failed, 0 skipped, 0 blacklisted
```

`MainActivity.kt` and `StateRestoreTest.kt` were compile-checked in the container with `kotlinc` 2.0.21 `-Werror`
against `android-all` 16 and stubs of the AndroidX, AndroidX Test and Qt 6.8 classes they use. The Android SDK and
Google Maven cannot be reached from the container, and CI's `android-build` does not compile `androidTest`.

### Emulator half (owed until the emulator CI of WI-0.035 / #487 runs it)

```console
cd android && ./gradlew :app:connectedFossDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=org.openvideotools.studio.StateRestoreTest
```

on the x86_64 or arm64 API 36 emulator started with `-gpu host`. `StateRestoreTest` has two tests:

- `recreationRestoresFrameAndSelection`: `ActivityScenario.recreate()` runs the same path "Don't keep activities"
  runs (`onSaveInstanceState`, `onDestroy` while not finishing, `onCreate` with the Bundle).
- `dontKeepActivitiesBackgroundAndResume` (acceptance test 1): sets `always_finish_activities` to 1, starts the app
  with a plain intent (ActivityScenario refuses to launch while "Don't keep activities" is on), puts frame 42/40
  and a byte selection into the QML root, goes to the launcher until the Activity is destroyed, starts the app again
  and asserts that the new Activity's own QML root publishes the same frame, selection, file and layout within
  2 000 ms. The setting is put back afterwards.

`MainActivity` logs `OVT: GUI state restored <n> ms after onCreate` for the record.

Mac Mini runs (emulator `ovt-api36-arm64`, API 36 arm64, 1080x2400, 420 dpi, `-gpu host`):

- f965fbf (2026-11-17, `wa455-pr730-staterestore-mini-20261117T045426Z.txt` in the project files): the androidTest
  APK built. `recreationRestoresFrameAndSelection` **PASS** (2.6 s; `OVT: GUI state restored 404 ms after
  onCreate`). `dontKeepActivitiesBackgroundAndResume` failed before the app ran: `ActivityScenario.launch()`
  throws `IllegalStateException` while "Don't keep activities" is on. The test now starts the app with an intent.
- Next head: _pending_.
