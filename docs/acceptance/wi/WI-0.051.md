# WI-0.051 acceptance record

Machine: cloud container, Linux x86_64, no Android SDK/NDK, no emulator, no Qt, no vcpkg network.
Date: 2026-11-14. Spec version: 1.0-rc25.

**Status: the emulator tests have not been run.** This container cannot build the APK or start an emulator, and
it has no Kotlin compiler, so the Kotlin sources and the host test below were reviewed but not compiled here.
The run on the emulator is owed before this work item is Done; the commands are below so that the Mac Mini
session or a CI emulator job can fill in the output.

## Test 1: pick `mini.mp4` through the SAF UI, logcat shows `OVT: opened fd size=<bytes>`

```console
$ cd android && ./gradlew :app:installFossDebug      # emulator API 36 running
$ adb push tests/data/mini.mp4 /sdcard/Download/      # any mini-corpus MP4
$ stat -c %s tests/data/mini.mp4
<bytes>
$ tools/android/uiautomator/pick_file.py mini.mp4
OVT: opened fd size=<bytes>                           # must equal the stat value
```

Output: NOT YET RECORDED.

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
