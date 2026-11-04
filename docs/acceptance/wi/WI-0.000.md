# WI-0.000 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1.
Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.000 pull request.

`CMakePresets.json` arrives with WI-0.001, so tests 2 and 3 configure the preset's build directory
directly (spec question #3). WI-0.001 re-runs them with `--preset`.

## Test 1: `ovt --version`

```console
$ cmake -S . -B build -G Ninja && cmake --build build
$ build/cli/ovt --version
ovt 0.1.0+f706e25 (core 0.1.0, api 1.0.0)
$ build/cli/ovt --version | grep -E '^ovt 0\.1\.0\+[0-9a-f]{7,}'
ovt 0.1.0+f706e25 (core 0.1.0, api 1.0.0)
```

Result: pass. The `(core …, api …)` tail follows the `--version` grammar of chapter 12 §12.2 and still
matches the regular expression.

## Test 2: licence manifest

```console
$ cmake -S . -B build/linux-x64-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
$ cmake --build build/linux-x64-debug --target ovt_license_manifest
[1/1] Writing build/linux-x64-debug/license_manifest.json
$ cat build/linux-x64-debug/license_manifest.json
{
  "options" :
  {
    "OVT_FFMPEG_GPL" : false,
    "OVT_FFMPEG_GPL_CORE_ACK" : false,
    "OVT_FFMPEG_GPL_PLUGIN_ONLY" : false,
    "OVT_PATENT_CODECS_IN_CORE" : false
  },
  "preset" : "linux-x64-debug",
  "schema_version" : 1,
  "targets" :
  [
    { "directory" : "cli/", "links" : [], "name" : "ovt_cli", "spdx" : "GPL-3.0-or-later", "type" : "EXECUTABLE" },
    { "directory" : "core/", "links" : [], "name" : "ovt_core", "spdx" : "LGPL-2.1-or-later", "type" : "SHARED" }
  ]
}
```

(Targets reformatted to one line each here.) Validated against the chapter 13 §13.2.3 schema with a
script that checks the top-level keys, `schema_version == 1`, boolean options, the target keys, the
`source` and `linkage` enums of every link, and the two expected licences. Result: pass.

The transitive walk was also exercised in a throwaway copy of the tree with an imported static library
`fake::fmt` (tagged `SOURCE vcpkg SPDX MIT VERSION 11.0.2`, interface link `m`) linked privately into
`ovt_core`: both `ovt_core` and `ovt_cli` listed `fake::fmt` (static, MIT) and `m` (system,
NOASSERTION).

## Test 3: GPL FFmpeg guard

```console
$ cmake -S . -B build/t3 -G Ninja -DOVT_FFMPEG_GPL=ON
CMake Error at cmake/OvtOptions.cmake:90 (message):
  OVT_FFMPEG_GPL=ON would link a GPL FFmpeg into the LGPL-2.1-or-later
  ovt_core.  Set OVT_FFMPEG_GPL_PLUGIN_ONLY=ON to apply --enable-gpl to the
  plugin flavour only (intended use), or OVT_FFMPEG_GPL_CORE_ACK=ON to accept
  a GPL-3.0-or-later core (never in official builds).
$ echo $?
1
$ cmake -S . -B build/t3b -G Ninja -DOVT_FFMPEG_GPL=ON -DOVT_FFMPEG_GPL_PLUGIN_ONLY=ON; echo $?
0
```

Result: pass.

## Additional checks

| Check | Result |
|---|---|
| `-DOVT_WERROR=ON` build, GCC 13 Debug and Release, Clang 18 Debug and RelWithDebInfo | 0 warnings |
| `-DOVT_ENABLE_SANITIZERS=ON` (asan-ubsan) with GCC 13, `ovt --version` | pass (the container lacks Clang's ASan runtime) |
| Exported symbols of `libovt_core.so` (`nm -D --defined-only`) | only the 8 `ovt_*` functions; SONAME `libovt_core.so.1` |
| `cmake --install` then a C11 consumer with `find_package(ovt 0.1 CONFIG REQUIRED)` and `ovt::core`, built with `-Wall -Wextra -Wpedantic -Werror` | builds and runs; `OVT_PLUGIN_DIR`, `OVT_SCHEMA_DIR` set |
| Two clean Release builds in the same build directory | byte-identical `libovt_core.so.1.0.0` and `ovt` |
| `ovt_license_manifest` run twice | byte-identical manifest |
| `ovt` with no argument / unknown option / `--help` | exit 2 / exit 2 / exit 0 |
