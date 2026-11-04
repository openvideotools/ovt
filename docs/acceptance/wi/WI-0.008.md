# WI-0.008 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1.
Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.008 pull request.

The OVT build used `-DOVT_USE_VCPKG=OFF` (system packages, see `docs/acceptance/wi/WI-0.002.md`). The
consumer is configured with no toolchain file and `CMAKE_PREFIX_PATH` set only to the install prefix, as
the test requires, whatever the outer build used.

## Test 1: `ctest -R downstream_consumer` on macOS and Linux

Linux, GCC 13 Debug:

```console
$ ctest --preset linux-x64-debug -R downstream_consumer -V
...
9: -- ovt package 1.0.0 from .../tests/downstream/work/prefix/lib/cmake/ovt; OVT_PLUGIN_DIR=.../work/prefix/lib/ovt/plugins
9: -- Build files have been written to: .../tests/downstream/work/build
9: -- ovt_version_string: 0.1.0
9: -- downstream_consumer: OK
1/1 Test #9: downstream_consumer ..............   Passed    0.27 sec
```

Linux, Clang 18 RelWithDebInfo with `-DOVT_WERROR=ON`:

```console
$ ctest --test-dir /tmp/bwc -R downstream
1/1 Test #8: downstream_consumer ..............   Passed    0.38 sec
```

The test (`tests/downstream/run_downstream.cmake`) installs the build into a scratch prefix, fails if
`INTERFACE_LINK_LIBRARIES` of the exported targets names any build-time dependency (`fmt::`, `spdlog::`,
…, `vcpkg_installed`, `ovt::flags`), configures `tests/downstream/` as a separate C11 project with
`find_package(ovt 1 CONFIG REQUIRED)`, links `ovt::core`, runs it and requires
`ovt_version_string: <version>` in its output.

macOS: not run here. The test carries the label `fast`, so the `pr-macos` lane runs it in
`build-macos-arm64` from WI-0.030 on.

Result: pass on Linux; macOS pending on CI.
