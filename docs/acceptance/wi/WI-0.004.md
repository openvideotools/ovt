# WI-0.004 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1.
Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.004 pull request.

Two substitutions, both because of this container, not the code:

- **GCC instead of `clang-16`.** The `linux-x64-asan-ubsan` preset names `clang-16`/`clang++-16` (the
  apt.llvm.org packages of the CI image). The container has Clang 18 without the ASan runtime, so the
  compiler is overridden with `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`. The flag sets are the
  same for both compilers.
- **System packages instead of vcpkg** (`-DOVT_USE_VCPKG=OFF`), because the session's network refuses
  vcpkg's source downloads (see `docs/acceptance/wi/WI-0.002.md`).

`cmake --workflow --preset ci-linux-asan-ubsan` cannot take `-D` overrides, so its three steps were run
one by one with the same presets. The `asan-ubsan` CI job of WI-0.031 runs the workflow preset itself.

## Test 1: the asan-ubsan workflow passes and the canary fails under ASan

```console
$ cmake --preset linux-x64-asan-ubsan -DOVT_USE_VCPKG=OFF -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
-- OVT sanitizers: asan-ubsan (-fsanitize=address,undefined;-fno-sanitize-recover=all;-fno-omit-frame-pointer;-O1;-g)
...
$ cmake --build --preset linux-x64-asan-ubsan
[11/11] Linking CXX executable core/tests/ovt_tests_core
$ ctest --preset sanitizers
...
100% tests passed, 0 tests failed out of 1
$ ctest --test-dir build/linux-x64-asan-ubsan -L canary --output-on-failure
...
==4403==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x502000000018 ...
SUMMARY: AddressSanitizer: heap-buffer-overflow /ovt/tests/canary/asan_canary.cpp:17 in main
The following tests FAILED:
	  2 - test_asan_canary (Failed)
```

Result: pass (workflow steps green; canary fails under ASan as intended).

## Test 2: only the canary runs with `-L canary`, and the sanitizers lane never selects it

```console
$ ctest --test-dir build/linux-x64-asan-ubsan -L canary -N
  Test #2: test_asan_canary

Total Tests: 1
$ ctest --preset sanitizers -N
  Test #1: version entry points

Total Tests: 1
$ ctest --test-dir build/linux-x64-debug -L canary -N      # not built without ASan
Total Tests: 0
```

Result: pass.

## Label mapping (step 3, used by WI-0.550)

```console
$ cat build/linux-x64-debug/core/tests/ovt_tests_core_ovt_tests.cmake
add_test([=[version entry points]=] [=[/home/claude/ovt/build/linux-x64-debug/core/tests/ovt_tests_core]=] [=[version entry points]=] --reporter console)
set_tests_properties([=[version entry points]=] PROPERTIES WORKING_DIRECTORY [=[...]=] LABELS [=[unit;fast;version]=] SKIP_RETURN_CODE 4)
$ tools/ctest.sh pr
+ ctest --preset pr
...
100% tests passed, 0 tests failed out of 1
```

The Catch2 tags `[version][fast]` became the labels `unit;fast;version`, and the `pr` lane (label `fast`)
selects the test.
