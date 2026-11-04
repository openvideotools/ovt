# WI-0.001 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, ccache 4.9.1.
Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.001 pull request.

The `base` preset names `third_party/vcpkg/scripts/buildsystems/vcpkg.cmake` as its toolchain file, and
that submodule arrives with WI-0.002. Until then every `--preset` configure on this branch overrides the
toolchain on the command line with `-DCMAKE_TOOLCHAIN_FILE= -DOVT_USE_VCPKG=OFF` (spec question #6).
WI-0.002 re-runs test 2 as written.

`CMakePresets.json` is the §13.2.4 JSON block, extracted byte for byte from
`docs/spec/13-platforms-build-ci.md` (the fenced block after the `### 13.2.4` heading).

## Test 1: macOS lists only the macOS presets

Needs a macOS host. Not run here; the Mac Mini runner covers it once WI-0.030's `build-macos-arm64` job
exists. The Linux half of the condition logic was checked instead:

```console
$ cmake --list-presets=configure | grep -c macos
0
$ cmake --list-presets=configure
Available configure presets:

  "linux-x64-debug"
  "linux-x64-release"
  "linux-arm64-release"
  "linux-x64-asan-ubsan"
  "linux-x64-tsan"
  "linux-x64-fuzz"
  "linux-x64-distro"       - Linux using system libraries (packagers)
  "android-arm64-debug"
  "android-arm64-release"
  "android-x86_64-debug"
  "android-x86_64-release"
```

Result: pending on macOS; the Linux host hides every `macos-*` configure preset as expected.

## Test 2: configure without Qt

Ubuntu 24.04 rather than 22.04 (the container image). `QT_ROOT_DIR` unset.

```console
$ env -u QT_ROOT_DIR cmake --preset linux-x64-debug -DCMAKE_TOOLCHAIN_FILE= -DOVT_USE_VCPKG=OFF
Preset CMake variables:
  ...
CMake Warning at cmake/OvtOptions.cmake:131 (message):
  Qt 6 not found (set QT_ROOT_DIR or CMAKE_PREFIX_PATH); building with
  OVT_BUILD_GUI=OFF.
...
-- Build files have been written to: /home/claude/ovt/build/linux-x64-debug
$ echo $?
0
```

Result: pass (with the toolchain override of #6).

## Test 3: test presets on Linux

```console
$ cmake --list-presets=test | grep -E '"(pr|nightly|sanitizers|perf|linux-x64-asan-ubsan)"'
  "linux-x64-asan-ubsan"
  "pr"
  "nightly"
  "sanitizers"
  "perf"
$ grep -c '"linux-x64-asan"' CMakePresets.json
0
```

Result: pass.

## Test 4: `tools/ctest.sh pr` on macOS runs `pr-macos`

Needs a configured `macos-arm64-release` tree on macOS; not run here. The dispatcher's mapping was checked
with the host override it provides for this purpose:

```console
$ OVT_CTEST_HOST=Darwin OVT_CTEST_DRY_RUN=1 tools/ctest.sh pr
+ ctest --preset pr-macos
$ OVT_CTEST_DRY_RUN=1 tools/ctest.sh pr macos-arm64-release -- -j2
+ ctest --preset pr-macos -j2
$ OVT_CTEST_HOST=Linux OVT_CTEST_DRY_RUN=1 tools/ctest.sh pr
+ ctest --preset pr
$ OVT_CTEST_DRY_RUN=1 tools/ctest.sh nightly android-x86_64-debug; echo $?
tools/ctest.sh: lane 'nightly' has no Android preset (only pr-android exists)
2
$ tools/ctest.sh pr          # real run on Linux, after cmake --build --preset linux-x64-debug
+ ctest --preset pr
Test project /home/claude/ovt/build/linux-x64-debug
No tests were found!!!
```

Result: mapping verified on Linux; the real macOS run is pending on the Mac Mini.

## WI-0.000 tests 2 and 3 re-run with `--preset` (#3)

```console
$ cmake --build --preset linux-x64-debug --target ovt_license_manifest
[1/1] Writing /home/claude/ovt/build/linux-x64-debug/license_manifest.json
$ python3 -c "import json;d=json.load(open('build/linux-x64-debug/license_manifest.json'));print(d['preset'],[(t['name'],t['spdx']) for t in d['targets']])"
linux-x64-debug [('ovt_cli', 'GPL-3.0-or-later'), ('ovt_core', 'LGPL-2.1-or-later')]

$ cmake --preset linux-x64-debug -DCMAKE_TOOLCHAIN_FILE= -DOVT_USE_VCPKG=OFF -DOVT_FFMPEG_GPL=ON
CMake Error at cmake/OvtOptions.cmake:90 (message):
  OVT_FFMPEG_GPL=ON would link a GPL FFmpeg into the LGPL-2.1-or-later
  ovt_core.  Set OVT_FFMPEG_GPL_PLUGIN_ONLY=ON to apply --enable-gpl to the
  plugin flavour only (intended use), or OVT_FFMPEG_GPL_CORE_ACK=ON to accept
```

Result: both pass through the preset.
