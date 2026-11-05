# WI-0.083 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, flatc 2.0.8 (system
`libflatbuffers-dev` / `flatbuffers-compiler`; vcpkg downloads are blocked in this container).
Date: 2026-11-05. Spec version: 1.0-rc2. Commit: the head of the WI-0.083 pull request, on top of WI-0.081.

Configure: `cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF -DCMAKE_CXX_COMPILER_LAUNCHER=
-DCMAKE_C_COMPILER_LAUNCHER= -DOVT_SYSTEM_DAV1D=ON`, then `cmake --build --preset linux-x64-debug`.

## Test 1: touching `ovt_model.fbs` regenerates, nothing lands in the source tree

`tools/ci/test_regen.sh` records the mtimes of `generated/ovt_model_generated.h` and `generated/ovt_model.bfbs`
and the `git status` of the source tree. It then touches `core/schema/ovt_model.fbs`, rebuilds `ovt_core`, and
checks that both mtimes increased, that `git status` is unchanged, and that the configure-time
`kModelSchemaSha256` equals `tools/schema_hash.py`.

```console
$ tools/ci/test_regen.sh linux-x64-debug
...
[4/7] embed_bfbs: ovt_schemas -> ovt_schemas_bfbs.inc
test_regen: OK (build/linux-x64-debug/generated/ovt_model_generated.h and build/linux-x64-debug/generated/ovt_model.bfbs regenerated, source tree unchanged, schema hash cc30dfbbb8ce24dcf182763fd349f447658b90f99057ce806693e36645430c6f)
```

Result: pass.

Determinism (§0.7.5): the SHA-256 of `ovt_model.bfbs`, `ovt_container.bfbs`, `smoke.bfbs` and
`ovt_schemas_bfbs.inc` was the same before and after the regeneration. The `.bfbs` files hold only
schema-relative file names (`//ovt_model.fbs`, set by `--bfbs-filenames`), not checkout paths.

## Tests 2 and 3: automated

- Test 2: `ctest --preset pr -R test_bfbs_resources` (`core/tests/model/test_bfbs_resources.cpp`). Model and
  Container pass. Metrics is skipped until `core/schema/ovt_metrics.fbs` (WI-0.082) exists; after that the
  check is required. With a stand-in `ovt_metrics.fbs` (root `MetricsReport`) the Metrics case passed. The
  stand-in was then removed.
- Test 3: `ctest --preset pr -R test_fb_python_import` runs
  `python -c "import ovt.fb.FrameAnalysis as m; m.FrameAnalysis"` with
  `PYTHONPATH=build/<preset>/generated/python`. Passed.
