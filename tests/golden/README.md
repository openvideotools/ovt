# Golden tests

Goldens are the canonical JSON of an **oracle** (FFmpeg `trace_headers`, JM, HM, libaom `inspect`, MP4Box, mp4ff,
MediaInfo, libvmaf, …) for one corpus file, committed as `tests/golden/<area>/<id>.<tool>.json`. OVT's own output is
converted to the same canonical JSON and compared with it (chapter 14 §14.4.0). The oracle is a **lower bound**: every
field in the golden must be present and equal in OVT's output; fields only OVT reports are ignored.

## The six families

| Family | Shape | C++ writer (`ovt::test::to_canonical`, `core/tests/test_support/fb_normalize.h`) |
|---|---|---|
| `syntax-elements` | `[{unit, name, bit_offset, bit_length, value}]` sorted by `(unit, bit_offset)` | `StreamIndex` or one `SyntaxNode` |
| `header-struct` | `{"sps": {...}, "pps": {...}, "slice_header": {...}}` | `StreamIndex` or one `SyntaxNode` |
| `block-grid` | `{w4, h4, fields: {name: [int per 4×4 cell]}}` | `FrameAnalysis` (+ optional field list) |
| `box-tree` | `[{path, offset, size, fields}]` | `ContainerTree` |
| `sample-table` | `[{i, dts, cts, size, offset, sync}]` | `SampleTable` |
| `scores` | `{frames: [{i, <metric>}], pooled: {<metric>: {mean, harmonic_mean, min}}}` | `MetricsResult` |

The Python twins live in `tests/oracles/normalize/` (WI-0.555). Both sides print numbers the same way: integers as
integers, every other number with `%.6f` (`normalize_json`).

## Comparison

`core/tests/test_support/golden_compare.h`:

- objects: every golden key must exist in OVT's output (missing → difference, named by JSON path such as
  `$.frames[3].psnr_y`); extra OVT keys are ignored;
- arrays: same length, element by element;
- numbers are compared as numbers (`1`, `1.0` and `1e0` are equal); two numbers of which one is a double are equal when
  their `%.6f` forms are equal, or, with a tolerance, when they differ by at most the tolerance;
- tolerances: a JSON file `{"frames[].psnr_y": 0.001, "default": 0}`; patterns are paths without the leading `$.` and
  with array indices written `[]`.

A mismatch prints the differing paths followed by a unified diff of the golden against OVT's normalised output.

## Registering a golden test

```cmake
ovt_add_golden_test(
  NAME h264_ba1_sony_d_trace_headers          # CTest name: golden_<NAME>; scratch output is unique per NAME
  AREA h264 ID jvt_BA1_Sony_D ORACLE trace_headers   # golden = tests/golden/h264/jvt_BA1_Sony_D.trace_headers.json
  FAMILY syntax-elements
  CORPUS jvt_BA1_Sony_D                       # manifest id; "@CORPUS@" in CMD becomes the verified file
  TOLERANCES ${CMAKE_CURRENT_SOURCE_DIR}/h264/tolerances.json   # optional
  CMD $<TARGET_FILE:ovt> syntax @CORPUS@ --format json ...)     # must print the canonical JSON on stdout
```

The command is any program that prints the canonical JSON of the family on stdout. A test with `CORPUS` is skipped
(ctest "Not Run") when the corpus file has not been fetched, and fails when `OVT_REQUIRE_CORPORA=1`. Labels: `golden`,
`fast` (`SLOW` → `slow`), `needs_corpus`, plus `LABELS`.

Run them with `ctest --preset linux-x64-debug -L golden` (add `-j8`).

## Updating goldens

`OVT_UPDATE_GOLDEN=1 ctest -L golden` rewrites each golden with OVT's normalised output instead of comparing. Use it
only when the oracle changed too, and say why in the PR. A golden generated this way is OVT's own output, not an
oracle's; the oracle image regenerates the real ones.

## The first golden test

`isobmff/ovt_min_structural.mp4` (2 KiB, committed) is walked by `isobmff/box_tree_main.cpp` into a `ContainerTree`
FlatBuffer, converted by `to_canonical`, and compared with `isobmff/ovt_min_structural.pybox.json`, written by the
independent Python walker `isobmff/gen_min_mp4.py` (`python3 tests/golden/isobmff/gen_min_mp4.py --check`). It proves
the plumbing without the container parser; when that exists the test moves to `ovt probe`.
