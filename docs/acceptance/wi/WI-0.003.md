# WI-0.003 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, system flatc and
libflatbuffers 2.0.8 (`-DOVT_USE_VCPKG=OFF`). CI covers the vcpkg configuration (flatbuffers 25.12.19).
Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.003 pull request.

## Test 1: generated files, Python import, schema smoke test

```console
$ cmake -S . -B build/linux -G Ninja -DOVT_USE_VCPKG=OFF -DOVT_BUILD_GUI=OFF -DOVT_BUILD_RENDER=OFF
$ cmake --build build/linux --target ovt_schemas
[1/1] flatc: ovt_model.fbs -> C++, Python, Kotlin
$ ls build/linux/generated/ovt_model_generated.h build/linux/generated/python/ovt/fb/FrameAnalysis.py
$ PYTHONPATH=build/linux/generated/python python3 -c 'import ovt.fb.FrameAnalysis' && echo ok
ok
$ cmake --build build/linux && ctest --test-dir build/linux
100% tests passed, 0 tests failed out of 12
```

Automated: `schema_python_import` (CTest, the import above) and `core/tests/schema/test_schema_smoke.cpp`
(`ovt_tests_schema_smoke`: build, verify, JSON, rebuild). Result: pass. The first file is a stub
(`ovt_model.fbs`) until WI-0.080 replaces it.

## Test 2: removed field

Automated: `tools/tests/test_check_schema_compat.py::test_removed_field_exits_1_and_names_it`. By hand:

```console
$ python3 -I tools/check_schema_compat.py --baseline-dir base x.fbs     # x.fbs without field T.b
E-REMOVED x.fbs T.b
E-CONFORM x.fbs /usr/bin/flatc: error: schemas don't conform: offsets differ for field: c
rc=1
```

Result: pass. flatc alone reports the next field (`c`), so the script names the removed one itself.

## Determinism

Two separate build trees produced byte-identical generated files (sha256 compared).
