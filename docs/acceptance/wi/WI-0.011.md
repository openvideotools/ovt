# WI-0.011 acceptance record

Machine: owner's Mac Mini (self-hosted build host), Apple M4, macOS 15.7.3 arm64, Apple clang 17.0.0,
CMake 4.4.5, Meson 1.5.2 (repo `.venv`), Android NDK 27.2.12479018.
Date: 2026-11-05. Spec version: 1.0-rc2. Commit: `main` at `c9478f1` (#45 merged, dav1d SHA-256 from #51).
dav1d 1.5.1 from the pinned archive, verified against its SHA-256 by the download step.

Environment for every run:

```sh
export QT_ROOT_DIR=$HOME/Qt/ovt/6.8.3 QT_HOST_KIT=macos VCPKG_ROOT=$PWD/third_party/vcpkg
export ANDROID_SDK_ROOT=$HOME/Library/Android/sdk ANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/27.2.12479018
```

## Test 1: `libdav1d.a` in the prefix and the probe test links (macOS)

```console
$ cmake --preset macos-arm64-debug && cmake --build --preset macos-arm64-debug
...
[33/35] Completed 'tp_dav1d'
[35/35] Linking CXX executable core/tests/ovt_tests_dav1d_probe
$ ls -l build/third_party/macos-arm64-debug/dav1d/lib/libdav1d.a
-rw-r--r--  1 ovt-build  staff  1022824 Nov  4 21:21 build/third_party/macos-arm64-debug/dav1d/lib/libdav1d.a
$ lipo -info build/third_party/macos-arm64-debug/dav1d/lib/libdav1d.a
Non-fat file: ... is architecture: arm64
$ ctest --test-dir build/macos-arm64-debug -R dav1d
1/2 Test #8: dav1d_version reports the linked library ...   Passed
2/2 Test #9: dav1d opens and closes a decoder context ...   Passed
```

`otool -hv` on the 58 extracted members: all `ARM64`. The `macos-arm64-asan` tree also builds `libdav1d.a` and
links the probe (part of the 54 passing tests of #56). Result: pass for macOS.

Android: `libdav1d.a` is produced in both prefixes (test 2 below). The probe test is not built there: `vcpkg.json`
excludes Catch2 on Android (`"platform": "!android"`), so the Android presets configure only with
`-DOVT_BUILD_TESTS=OFF` (without it configure stops at `ovt_find_package: Catch2 not found`). Linking `tp::dav1d`
into an Android test is left to the Android test harness (WI-0.034).

The two Linux presets are not covered here; CI's `build-linux-x64` builds them.

## Test 2: Android members report AArch64 / X86-64

```console
$ cmake --preset android-arm64-release -DOVT_BUILD_GUI=OFF -DOVT_BUILD_TESTS=OFF
$ cmake --build --preset android-arm64-release --target tp_dav1d
$ cmake --preset android-x86_64-debug -DOVT_BUILD_GUI=OFF -DOVT_BUILD_TESTS=OFF
$ cmake --build --preset android-x86_64-debug --target tp_dav1d
$ R=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf   # LLVM 18.0.3
$ $R -h build/third_party/android-arm64-release/dav1d/lib/libdav1d.a | grep Machine: | sort | uniq -c
  71   Machine:                           AArch64
$ $R -h build/third_party/android-x86_64-debug/dav1d/lib/libdav1d.a | grep Machine: | sort | uniq -c
  98   Machine:                           Advanced Micro Devices X86-64
```

Every member reports the expected machine (`llvm-readelf` spells X86-64 as "Advanced Micro Devices X86-64").
Result: pass.
