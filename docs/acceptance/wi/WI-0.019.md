# WI-0.019 acceptance record

Test 1 is automated (CTest `ovt_studio_self_test`, label `fast`, run by the `build-linux-x64` and
`build-macos-arm64` CI jobs). Tests 2 and 3 need a Mac and the Android kits; they are recorded here after
the run on the owner's Mac Mini.

## Test 1: `ovt-studio --self-test` with `QT_QPA_PLATFORM=offscreen`

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, system Qt 6.4.2 (the pinned Qt
6.8.3 download is blocked in the container; CI uses 6.8.3). Date: 2026-11-05.

```console
$ cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF -DOVT_WERROR=ON ...
$ cmake --build --preset linux-x64-debug
$ time QT_QPA_PLATFORM=offscreen build/linux-x64-debug/app/ovt-studio --self-test; echo rc=$?
OVT Studio: self-test passed (first frame, platform offscreen, core 0.1.0)
rc=0
real    0m0.067s
$ ctest --preset pr
100% tests passed, 0 tests failed out of 56
```

Result: pass (0.07 s, budget 5 s). The CTest test has `TIMEOUT 5`.

## Test 2: macOS, ad-hoc-signed `OVT Studio.app` shows the main window

Machine: the owner's Mac Mini (M4, macOS 15.7.3, Xcode 26.3 with SDK 26.2, CMake 4.4.5, Qt 6.8.3 from
`tools/install_qt.py`), user `ovt-build`. Date: 2026-11-05. Commit: efe106e, plus the two fixes this PR then
received (the `Info.plist.in` comment, and the AGL stand-in in `cmake/OvtOptions.cmake`).

```console
$ cmake --workflow --preset ci-macos-arm64        # with the two fixes applied by hand
100% tests passed, 0 tests failed out of 56
$ codesign --force --deep --sign - "build/macos-arm64-release/app/OVT Studio.app"   # OK
$ open "build/macos-arm64-release/app/OVT Studio.app"
launchd error 125 (ovt-build has no GUI session)
$ QT_QPA_PLATFORM=offscreen "build/macos-arm64-release/app/OVT Studio.app/Contents/MacOS/OVT Studio" --self-test
OVT Studio: self-test passed (first frame, platform offscreen, core 0.1.0)
rc=0 (0.02 s)
```

Bugs found by this run and fixed in this PR:

1. `configure_file` expanded the `${MACOSX_BUNDLE_*}` text in the header comment of `Info.plist.in`, and
   CMake failed at generate time.
2. Xcode 26's SDK has no AGL framework, but Qt 6.8.3's `FindWrapOpenGL.cmake` links it, so the link failed
   with `ld: framework 'AGL' not found`. `cmake/OvtOptions.cmake` now points `WrapOpenGL_AGL` at
   `OpenGL.framework` when AGL is missing.

Result: **partial**. The bundle builds, signs and passes the offscreen self-test. Nobody has yet seen the
main window appear, because `ovt-build` has no desktop session. That check needs Karl logged in to
`ovt-build`'s desktop (or the bundle opened from his own session).

## Test 3: Android presets produce the library for both ABIs

Machine: the Mac Mini as above, Android NDK 27.2.12479018, Qt 6.8.3 `android_arm64_v8a` and `android_x86_64`
kits. Date: 2026-11-05. Commit: efe106e.

```console
$ cmake --preset android-arm64-debug -DOVT_BUILD_TESTS=OFF -DOVT_BUILD_GUI=ON -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT
$ cmake --build --preset android-arm64-debug
build/android-arm64-debug/app/libovt_studio_arm64-v8a.so   1,863,656 bytes, ELF AArch64
$ cmake --preset android-x86_64-debug -DOVT_BUILD_TESTS=OFF -DOVT_BUILD_GUI=ON -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT
$ cmake --build --preset android-x86_64-debug
build/android-x86_64-debug/app/libovt_studio_x86_64.so     1,834,024 bytes, ELF x86-64
```

Both builds also wrote `ovt_studio.apk`.

Result: pass. Qt names the library `libovt_studio_<ABI>.so` (spec question #71). The run needed three
configure flags that the Android presets do not set:

- `ANDROID_SDK_ROOT` as a cache variable. Setting it only in the environment fails in `Qt6AndroidMacros`
  with "Could not locate Android SDK build tools".
- `OVT_BUILD_TESTS=OFF`.
- `OVT_BUILD_GUI=ON`.

`CMakePresets.json` is protected, so this PR leaves the presets as they are and notes the change for Karl.
