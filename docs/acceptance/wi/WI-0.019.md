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

Pending: run on the Mac Mini (`cmake --workflow --preset ci-macos-arm64`, `codesign --force --deep --sign -`,
`open "build/macos-arm64-release/app/OVT Studio.app"`).

## Test 3: Android presets produce the library for both ABIs

Pending: run on the Mac Mini with the Android kits (`android-arm64-debug`, `android-x86_64-debug`). Qt names
the library `libovt_studio_<ABI>.so` (spec question #71).
