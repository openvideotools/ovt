# WI-0.031 acceptance record

Machine: the Mac Mini CI runner (self-hosted, macos-arm64), Apple clang. Date: 2026-11-06. Spec version: 1.0.0-rc2.

## Test 1: UB in a test fails `asan-ubsan`

Run on the macOS lane (`asan-ubsan-macos`, `ci-macos-asan`), because the Linux `asan-ubsan` job is skipped while
`OVT_HOSTED_RUNNERS=off`. A throwaway commit `cb03f00` added a test case containing `1 << 33` to
`core/tests/test_version.cpp`; the commit was reverted in `f65a5d7`.

```console
core/tests/test_version.cpp:57:26: runtime error: shift exponent 33 is too large for 32-bit type 'int'
The following tests FAILED:
	  8 - throwaway UB for WI-0.031 AT1 (Subprocess aborted) fast unit version
##[error]Process completed with exit code 8.
```

Run: https://github.com/openvideotools/ovt/actions/runs/42174498262/job/127749779284 (red, names the test).
The Linux `asan-ubsan` job (Clang 16) is untested until hosted runners return.

## Test 2: removed `ovt_*` symbol fails `abi-check` — deferred

`abi-check` is Linux-only (libabigail) and needs a `v*` tag as baseline. Neither exists yet (hosted runners are off,
no release has been cut), so the job has never run. To do after the first tag: remove a symbol from a throwaway
branch and confirm the job fails with the abidiff report attached.

## Test 3: deleting the `bitreader/` tests fails `coverage`

- Gate logic: `tools/tests/test_coverage_gate.py::test_low_bitreader_names_directory_and_percentage` and
  `test_flags_prefix_map_names_are_remapped` (export names as the Mini produces them, `/ovt/core/src/...`).
- Job: `coverage` runs on pushes to `main` and nightly only (the Mini is CPU-bound); a `workflow_dispatch` trigger is in
  `ci.yml`, but GitHub offers it only once the file is on `main`. The first green run, and a red run with the
  bitreader tests removed, are to be recorded after the merge.
