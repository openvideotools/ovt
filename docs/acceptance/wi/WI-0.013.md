# WI-0.013 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, Ninja, system packages per the local-build
recipe (`OVT_USE_VCPKG=OFF`, `OVT_SYSTEM_DAV1D=ON`). Date: 2026-11-06. Spec version: 1.0-rc2. Commit: the head of the
WI-0.013 pull request. macOS arm64 is covered by the `build-macos-arm64` CI job on the Mac Mini.

## Test 1: probe test on the platform presets

Automated: `ovt_tests_aom_probe` (`core/tests/test_aom_probe.cpp`), test cases
`aom_codec_version_str matches the VERSIONS.md pin` and `libaom accepts AV1_SET_INSPECTION_CALLBACK`, both `[fast]`.

```console
$ cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF -DOVT_SYSTEM_DAV1D=ON \
    -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
-- OVT: libaom AOM_TARGET_CPU=x86_64
-- Downloading libaom 3.12.1: https://storage.googleapis.com/aom-releases/libaom-3.12.1.tar.gz
$ cmake --build --preset linux-x64-debug
$ ctest --preset linux-x64-debug -R aom
1/3 Test #30: aom_codec_version_str matches the VERSIONS.md pin ...   Passed    0.01 sec
2/3 Test #31: libaom accepts AV1_SET_INSPECTION_CALLBACK ..........   Passed    0.00 sec
3/3 Test #32: aom_inspection_check ................................   Passed    0.76 sec
$ ctest --preset pr
100% tests passed, 0 tests failed out of 226
```

Result: pass on linux-x64. macos-arm64: CI job `build-macos-arm64` (`pr-macos`). The Android presets and
linux-arm64 have no runner yet (WI-0.034); `OVT_AOM_CPU` resolves to `arm64` for `arm64-v8a`/`aarch64` and to `x86_64`
for `x86_64`, and libaom gets the NDK toolchain through `OVT_TP_CMAKE_TOOLCHAIN_ARGS`. They are checked when that job
lands.

`OVT_BUILD_AOM_INSPECT=ON` (interface line): `cmake --build build/linux-x64-debug --target tp_libaom` prints
`Installing libaom's inspect oracle`; `build/third_party/linux-x64-debug/libaom/bin/inspect` runs and prints its usage.

## Test 2: system libaom without inspection

Automated: CTest `aom_inspection_check` (`core/tests/aom_inspection_check/run_check.cmake`, labels `fast`,
`third_party`). It configures a one-file project that calls the `OVT_SYSTEM_LIBAOM` check twice: against
`fake_libaom.c` (answers `AV1_SET_INSPECTION_CALLBACK` with `AOM_CODEC_INCAPABLE`, as libaom does without
`CONFIG_INSPECTION`), where configure must fail with the message, and against the bundled `libaom.a`, where it must pass.

Manual, against the real distro library (Ubuntu 24.04 `libaom-dev` 3.8.2, built without inspection):

```console
$ cmake -S . -B /tmp/b-sys -G Ninja -DOVT_USE_VCPKG=OFF -DOVT_SYSTEM_DAV1D=ON -DOVT_SYSTEM_LIBAOM=ON \
    -DOVT_BUILD_PLUGIN_DE265=OFF -DOVT_BUILD_GUI=OFF
CMake Error at cmake/OvtAomInspection.cmake (message):
  libaom was not built with CONFIG_INSPECTION (AV1_SET_INSPECTION_CALLBACK
  returned 4).  Build it with -DCONFIG_INSPECTION=1 -DCONFIG_ACCOUNTING=1, or
  use the bundled libaom (OVT_SYSTEM_LIBAOM=OFF).
```

Result: pass. The check builds and runs the probe instead of only compiling it (spec question #152).

## Licence: no SVT-AV1 code

`third_party/libaom.cmake` passes `-DCONFIG_SVT_AV1=0` (owner decision, 2026-11-08), so libaom's AVX2 convolution
does not include the BSD-3-Clause-Clear sources of its `third_party/SVT-AV1`. Checked on 2026-11-08 by configuring
libaom 3.12.1 with the script's flags: `config/aom_config.h` has `#define CONFIG_SVT_AV1 0` and `libaom.a` builds.
