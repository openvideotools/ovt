# CMake presets

`CMakePresets.json` at the repository root is the single entry point for every build (chapter 13 §13.2.4,
D23). Its preset names are fixed by the specification: do not add, rename or remove presets in this file.
Local tweaks go into `CMakeUserPresets.json` (see below).

## Which preset to use

| Host | Day-to-day | CI-equivalent release | Sanitizers |
|---|---|---|---|
| Linux x86_64 | `linux-x64-debug` | `linux-x64-release` | `linux-x64-asan-ubsan`, `linux-x64-tsan` |
| Linux arm64 | — | `linux-arm64-release` | — |
| macOS arm64 | `macos-arm64-debug` | `macos-arm64-release` | `macos-arm64-asan` |
| macOS x86_64 | `macos-x86_64-debug` | `macos-x86_64-release` | — |
| Android (cross, from Linux or macOS) | `android-arm64-debug`, `android-x86_64-debug` | `android-*-release` | — |

Other configure presets: `linux-x64-fuzz` (libFuzzer targets, Clang only) and `linux-x64-distro` (system
libraries, no vcpkg; for packagers).

`cmake --list-presets` shows only the presets that apply to the host: the macOS presets carry a
`Darwin` condition and the Linux presets a `Linux` condition.

```sh
cmake --preset linux-x64-debug           # configure into build/linux-x64-debug/
cmake --build --preset linux-x64-debug   # build
tools/ctest.sh pr                        # run the fast lane for this host
```

Every configure preset writes to its own `build/<preset>/` directory, which keeps sanitizer, fuzz and
release trees apart. vcpkg packages are shared across presets in `build/vcpkg_installed/`; third-party
builds go to `build/third_party/<preset>/`.

`debug` presets build with `OVT_WERROR=OFF` and no LTO; `release` presets use `RelWithDebInfo`, LTO and
`OVT_WERROR=ON`, like CI.

## Test presets and lanes

A CMake test preset is bound to exactly one configure preset, so a host-agnostic `pr` preset cannot exist.
The specification's `ctest --preset pr` means the lane preset of the host:

| Lane | Linux | macOS | Android | Selects |
|---|---|---|---|---|
| `pr` | `pr` (on `linux-x64-debug`) | `pr-macos` (on `macos-arm64-release`) | `pr-android` (on `android-x86_64-debug`) | label `fast`, minus `needs_display`, `needs_android`, `canary` |
| `nightly` | `nightly` (on `linux-x64-release`) | `nightly-macos` | — | everything except `canary` |
| `sanitizers` | `sanitizers` (on `linux-x64-asan-ubsan`) | `sanitizers-macos` (on `macos-arm64-asan`) | — | `fast` and `golden`, minus `needs_display`, `needs_android`, `canary`, `no_sanitizer` |
| `perf` | `perf` (on `linux-x64-release`) | `perf-macos` | — | label `perf`, one job at a time |

`tools/ctest.sh <pr|nightly|sanitizers|perf> [configure-preset] [-- ctest args]` picks the right name:

```sh
tools/ctest.sh pr                          # Linux: ctest --preset pr; macOS: ctest --preset pr-macos
tools/ctest.sh pr macos-arm64-release      # ctest --preset pr-macos
tools/ctest.sh nightly -- -R version       # extra arguments after -- go to ctest
```

The lane's configure preset must have been configured and built first: `pr` reads
`build/linux-x64-debug/`, while `pr-macos` reads `build/macos-arm64-release/`.

There is also one test preset per native configure preset (`linux-x64-debug`, `macos-arm64-release`, …)
that runs every test of that tree. JUnit output goes to `build/<test-preset>/junit.xml`.

Workflow presets chain configure, build and test: `ci-linux-x64`, `ci-macos-arm64`,
`ci-linux-asan-ubsan`, `ci-macos-asan`.

```sh
cmake --workflow --preset ci-linux-x64
```

## Environment variables the presets read

| Variable | Used by | Example |
|---|---|---|
| `QT_ROOT_DIR` | desktop and Android presets (`CMAKE_PREFIX_PATH`) | `~/Qt/6.8.3` |
| `QT_HOST_KIT` | Android presets (`QT_HOST_PATH`) | `macos` or `gcc_64` |
| `ANDROID_NDK_ROOT` | Android presets (chain-loaded toolchain) | `~/Android/Sdk/ndk/27.2.12479018` |
| `ANDROID_SDK_ROOT`, `JAVA_HOME` | Gradle side of the Android build | |

When `QT_ROOT_DIR` is unset on a desktop host, configure still succeeds: it prints a warning and builds
with `OVT_BUILD_GUI=OFF`.

## vcpkg triplets

The presets select the custom triplets in `third_party/vcpkg-triplets/`:

| Triplet | Preset family |
|---|---|
| `arm64-osx-ovt` | `macos-arm64-*` |
| `x64-osx-ovt` | `macos-x86_64-*` |
| `x64-linux-ovt` | `linux-x64-*` |
| `arm64-linux-ovt` | `linux-arm64-*` |
| `arm64-android-ovt` | `android-arm64-*` |
| `x64-android-ovt` | `android-x86_64-*` |

All of them build static libraries (absorbed into `libovt_core`) against the dynamic C runtime, with
hidden visibility and position-independent code. The macOS triplets set the deployment target to 13.0. The
Android triplets set API level 28 through `VCPKG_CMAKE_SYSTEM_VERSION` and pass `-DANDROID_STL=c++_shared`.
The triplet files keep vcpkg's `x64-` spelling while the preset names use `x86_64`.

## Local overrides: `CMakeUserPresets.json`

Put machine-specific settings in `CMakeUserPresets.json` at the repository root. Git ignores it, and CMake
loads it together with `CMakePresets.json`, so its presets can inherit the official ones. Example: a debug
tree built with Clang and without ccache.

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "my-clang-debug",
      "inherits": "linux-x64-debug",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++",
        "CMAKE_C_COMPILER_LAUNCHER": "", "CMAKE_CXX_COMPILER_LAUNCHER": ""
      }
    }
  ],
  "buildPresets": [ { "name": "my-clang-debug", "configurePreset": "my-clang-debug" } ]
}
```

Never commit a `CMakeUserPresets.json`, and never edit `CMakePresets.json` for a local need.

## Requirements

Presets use schema version 6, which needs CMake 3.25 or later. `cmakeMinimumRequired` in the presets file
and `cmake_minimum_required` in the top-level `CMakeLists.txt` must stay equal. The `base` preset uses
`ccache` as the compiler launcher, so `ccache` must be installed (`tools/bootstrap.sh` checks for it).
