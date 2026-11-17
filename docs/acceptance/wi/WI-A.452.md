# WI-A.452 acceptance record

Machine: cloud container, Linux x86_64, no Android SDK/NDK and no emulator (Google's Maven and the SDK downloads are
blocked by the container's proxy). Date: 2026-11-17. Spec version: 1.0-rc41.

**Status: both tests owed on the emulator.** The instrumentation tests below need the API 36 emulator (the Mac
Mini, AVD `ovt-api36-arm64`, run with `-gpu host`). Until the Android instrumentation job of WI-1.577 exists, the
results are recorded here.

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

Output: owed.

## Test 2: a non-seekable provider raises `GUI-AND-0001` with the copy action, and the copy opens

Automated: `SafFlowsTest.nonSeekableDocumentRaisesGuiAnd0001AndOpensAfterCopy` (a `pipe-` document of the test
provider is a pipe; the `GUI-AND-0001` dialog appears, nothing reaches the core, "Copy to app storage" streams the
document to `getExternalFilesDir(null)/copies/` (SPEC-ASSUMPTION(#722)) and the copy opens and enters the recent
list). Supporting: `fstatDecidesSeekability`, `copyNamesAreSafeAndUnique`, `cancelledCopyLeavesNoFile`.

Output: owed.

## Export (step 3, no acceptance line)

`SafFlowsTest.exportCreatesAndWritesTheDocument`: the `ACTION_CREATE_DOCUMENT` intent carries the MIME type and the
suggested name, and the writer's bytes land in the created document. By hand: open a file, tap "Export info…",
save to Downloads, and check the JSON there.
