# WI-0.017 acceptance record

## Test 1: `lipo -archs` on `libMoltenVK.dylib` reports one architecture

Automated: Catch2 test case `libMoltenVK.dylib is a thin Mach-O of the built architecture` in
`render/tests/test_moltenvk_probe.cpp` (executable `ovt_tests_moltenvk_probe`). It reads the Mach-O header of
the dylib in the prefix and requires a thin 64-bit file of the architecture being built; a universal file
(`0xcafebabe`) fails it. It runs on the Mac Mini in the `build-macos-arm64` CI job (`pr-macos`, label `fast`).
The Linux container cannot run it (no macOS).

## Test 2: Linux x64, `-DOVT_ONNXRUNTIME_PREBUILT=ON`, `OrtGetApiBase()` probe exits 0

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13, CMake 3.28.3, Ninja 1.11.1; no vcpkg
(`OVT_USE_VCPKG=OFF`). Date: 2026-11-06. Spec version: 1.0-rc2.

```console
$ cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF -DOVT_ONNXRUNTIME_PREBUILT=ON -DOVT_SYSTEM_DAV1D=ON \
    -DOVT_BUILD_GUI=OFF -DCMAKE_CXX_COMPILER_LAUNCHER= -DCMAKE_C_COMPILER_LAUNCHER=
$ cmake --build build/linux-x64-debug --target ovt_tests_ort_probe
$ ctest --test-dir build/linux-x64-debug -R ort_probe -V
5: ONNX Runtime 1.20.1
1/1 Test #5: ort_probe ........................   Passed    0.01 sec
```

Result: pass. `onnxruntime_c_api.h` is found under `build/third_party/linux-x64-debug/onnxruntime/include`.
CTest registers the test `ort_probe` (labels `unit;fast;third_party`) only when `OVT_ONNXRUNTIME_PREBUILT=ON`.

## Addendum (#167): notices staging with `OVT_ONNXRUNTIME_PREBUILT=ON`

Machine: cloud container, Ubuntu 24.04 x86_64. Date: 2026-11-06.

```console
$ cmake --build build/linux-x64-debug --target ovt_notices   # fails on this container's system packages, see below
$ ls build/linux-x64-debug/notices/licenses/onnxruntime
LICENSE  ThirdPartyNotices.txt
$ python3 -I tools/gen_notices.py ... | grep -ci onnx    # E-NOTICE-MISSING lines naming ONNX Runtime
0
```

The remaining `E-NOTICE-MISSING` lines are apt-installed libraries (fmt, Catch2, …) that have no texts here; CI uses vcpkg.
