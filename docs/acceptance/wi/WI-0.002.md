# WI-0.002 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1.
Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.002 pull request.

**The vcpkg path could not run in this container.** `third_party/vcpkg/bootstrap-vcpkg.sh` succeeds, but
the session's network policy refuses source-archive downloads from github.com (HTTP 403 on CONNECT), so
`vcpkg install` stops at the first port:

```console
$ cmake --preset linux-x64-debug
...
-- Running vcpkg install
...
error: building catch2:x64-linux-ovt failed with: BUILD_FAILED
(vcpkg_from_github -> z_vcpkg_download_distfile: download refused)
```

The custom triplet `x64-linux-ovt` was resolved through `vcpkg-configuration.json`'s overlay before the
download failed. The vcpkg path is tested by CI from WI-0.030 on (`build-linux-x64` and
`build-macos-arm64` configure through these presets with a warm `files` cache). Until then, the same
CMake code was exercised against the Ubuntu system packages with `-DOVT_USE_VCPKG=OFF`, which resolves
every package through the same `ovt_find_package()` calls.

## Test 1: one test passes

```console
$ cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF
-- OVT dependency: fmt 9.1.0 (system, fmt::fmt)
-- OVT dependency: spdlog 1.12.0 (system, spdlog::spdlog)
-- OVT dependency: flatbuffers 2.0.8 (system, flatbuffers::flatbuffers)
-- OVT dependency: zstd 1.5.5 (system, zstd::libzstd_static)
-- OVT dependency: nlohmann_json 3.11.3 (system, nlohmann_json::nlohmann_json)
-- OVT dependency: hwy 1.0.7 (system, hwy::hwy)
-- OVT dependency: xxHash 0.8.2 (system, xxHash::xxhash)
-- OVT dependency: Catch2 3.4.0 (system, Catch2::Catch2WithMain)
-- OVT dependency: CLI11 2.4.1 (system, CLI11::CLI11)
-- Build files have been written to: /home/claude/ovt/build/linux-x64-debug
$ cmake --build --preset linux-x64-debug
[9/9] Linking CXX executable core/tests/ovt_tests_core
$ ctest --preset linux-x64-debug
    Start 1: version entry points
1/1 Test #1: version entry points .............   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1
```

Result: pass against system packages; the vcpkg run is pending on CI (WI-0.030).

## Test 2: warm binary cache, vcpkg step under 60 s

Not run here (needs the vcpkg downloads). CI measures it: the second `build-linux-x64` run of the WI-0.030
pull request restores `build/vcpkg-archives` and its configure log shows the vcpkg install time.

Result: pending on CI.

## Other checks

- `-DOVT_WERROR=ON`: 0 warnings with GCC 13 (Debug) and Clang 18 (Debug).
- `cmake --install` into a prefix: `ovt-targets.cmake` has no `INTERFACE_LINK_LIBRARIES`, so no vcpkg
  target leaks to consumers (WI-0.008 automates this).
- Licence manifest: every link of `ovt_core`, `ovt_cli` and `ovt_tests_core` has an SPDX id; none is
  `NOASSERTION`.
- WI-0.001 test 2 (`cmake --preset linux-x64-debug` exactly as written) now gets past the toolchain step;
  the full run waits for the same downloads.
