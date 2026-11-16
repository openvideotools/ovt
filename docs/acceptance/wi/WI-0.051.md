# WI-0.051 acceptance record

Machines: Test 1 on the Mac Mini (Mac16,10, Apple M4, macOS 15.8.2) with the Android SDK emulator, 2026-11-16, at
PR head 1285530. Test 2 and the checks at the end in a cloud container (Linux x86_64), 2026-11-14.
Spec version: 1.0-rc24.

## Test 1: pick `mini.mp4` through the SAF UI, logcat shows `OVT: opened fd size=<bytes>`

Run on the Mac Mini, 2026-11-16, PR #420 at 1285530. AVD "ovt-api36-arm64" (system image android-36 google_apis
arm64-v8a, pixel_6 profile, headless: `-no-window -gpu swiftshader_indirect`).

```console
$ cd android && ./gradlew :app:assembleFossDebug -Povt.abis=arm64-v8a
BUILD SUCCESSFUL in 31m 26s                           # both ABIs were built anyway (follow-up below)
$ adb install -r app-foss-arm64-v8a-debug.apk
Success
$ adb push mini.mp4 /sdcard/Download/                 # corpus mpegff_a1_foreman_qcif (a1-foreman-QCIF.mp4)
1 file pushed (475988 bytes)
$ stat -f %z mini.mp4
475988
$ adb logcat -s OVT                                   # at start
I OVT : ovt_jni loaded, core 0.1.0
I OVT : OVT: native {"version":"0.1.0","build":{"commit":"1285530","profile":"relwithdebinfo","platform":"android-arm64","features":[]}}
I OVT : OVT: QML root ready, title 'OVT Studio'
# tap "OPEN FILE…" -> DocumentsUI PickActivity ("Recent"), tap mini.mp4
11-15 21:30:58.644  7560  8837 I OVT     : opened fd size=475988
```

Output: PASS. The logged size equals the `stat` value. A screenshot after the pick is kept with the project's
shared files (`wi0051-emulator-after-pick.png`), not in the repository: it shows Android system UI.

The taps were made by hand with the same `adb shell input tap` and `uiautomator dump` steps that `pick_file.py` uses,
because the script did not complete on that run:

1. A system ANR dialog ("Messages isn't responding") covered the app on the freshly booted image, so the script did
   not find the button. It was dismissed with "Close app".
2. On API 36 the app is edge-to-edge, and the overlay's button (bounds [16,16][280,142] on 1080x2400) sat mostly
   under the status bar. The script's tap at its centre hit the status bar. Fixed after this run: `DebugInfoOverlay`
   now applies the system-bar and cutout insets as margins. The uppercase label in the dump ("OPEN FILE…") was
   already matched, because `pick_file.py` compares case-insensitively.

Follow-up outside this WI: `-Povt.abis=arm64-v8a` does not restrict the native build (WI-0.050 Gradle setup).

## Test 2: a 3 GB sparse file reports its 64-bit size

Automated on the host as `capi_file_open_fd_3gb_sparse` (`ctest --preset pr -R io`; `core/tests/io/test_io_fd.cpp`):
a 3 000 000 000 byte sparse file opened by descriptor, with the size given (as `getStatSize()`) and with 0.
The emulator variant is the same call chain through `ovt_jni_open_fd`; `OvtNative.fileSize(session)` returns
the value (`ovt_jni_file_size`). Output: NOT YET RECORDED.

## What was checked here

- `ovt_jni.cpp` compiles with `g++ -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -fsyntax-only`
  against the repository headers, a stub `android/log.h` and the JDK's `jni.h`. The only diagnostics are the
  JDK header's `char*` fields in `JNINativeMethod` (the NDK header uses `const char*`), which also affect the
  WI-0.050 code.
- `clang-format` is clean on the C++ files; `pick_file.py` byte-compiles and prints its usage.
