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

Run: <https://github.com/openvideotools/ovt/actions/runs/42174498262/job/127749779284> (red, names the test).
The Linux `asan-ubsan` job (Clang 16) is untested until hosted runners return.

## Test 2: removed `ovt_*` symbol fails `abi-check` — deferred

`abi-check` is Linux-only (libabigail) and needs a `v*` tag as baseline. Neither exists yet (hosted runners are off,
no release has been cut), so the job has never run. To do after the first tag: remove a symbol from a throwaway
branch and confirm the job fails with the abidiff report attached.

## Test 3: deleting the `bitreader/` tests fails `coverage`

Both runs are the `coverage` job on the Mac Mini (Apple clang, `macos-arm64-debug` with `OVT_COVERAGE=ON`).

- Green, `main` at `e65e490` (the merge of #112):
  <https://github.com/openvideotools/ovt/actions/runs/42186898783/job/127790104784>

  ```console
  core/src/: 93.4 % of 2421 lines (minimum 80 %)
  bitreader/: 100.0 % of 434 lines (minimum 90 %)
  syntax/: 94.0 % of 596 lines (minimum 90 %)
  container/: no source files yet; skipped
  ```

- Red, `workflow_dispatch` of `ci.yml` on a throwaway commit that comments out `add_subdirectory(bitreader)` in
  `core/tests/CMakeLists.txt` (reverted in the same pull request):
  <https://github.com/openvideotools/ovt/actions/runs/42189693372/job/127798386673>

  ```console
  core/src/: 90.4 % of 2421 lines (minimum 80 %)
  bitreader/: 83.2 % of 434 lines (minimum 90 %)
  ##[error]coverage below threshold: bitreader/: 83.2 % < 90 %
  ```

  The percentage does not drop to 0 %, because other test executables still call `BitReader`.

Unit tests of the gate logic: `tools/tests/test_coverage_gate.py`.
