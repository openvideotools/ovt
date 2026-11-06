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
| `block-grid` | `{w4, h4, fields: {name: [int per 4×4 cell]}}` | `FrameAnalysis` |
| `box-tree` | `[{path, offset, size, fields}]` | `ContainerTree` |
| `sample-table` | `[{i, dts, cts, size, offset, sync}]` | `SampleTable` |
| `scores` | `{frames: [{i, <metric>}], pooled: {<metric>: {mean, harmonic_mean, min}}}` | `MetricsResult` |

The Python twins are in `tests/oracles/normalize/`; both sides print non-integers with `%.6f`.

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

`OVT_UPDATE_GOLDEN=1 ctest -L golden` rewrites a golden with OVT's normalised output instead of comparing, but only
for tests registered `UPDATABLE` (OVT-side goldens, §14.4.0 step 4). An oracle golden (`<id>.<tool>.json` written by
an oracle in the oracle image) is the lower bound OVT is measured against; rewriting it with OVT's own output would make
every later run pass, so the run is refused. Update is also refused when `CI` is set. Use it only when the oracle
changed too, and say why in the PR.

## Limits of the comparison

A tolerance applies only when one number is a double (integers are exact). Without one, doubles are equal when their
`%.6f` forms are equal, so values below 5e-7 are all equal; tiny-valued families need a scaled field.

## The first golden test

`isobmff/ovt_min_structural.mp4` (2 KiB) is walked into a `ContainerTree` by `isobmff/box_tree_main.cpp`, converted by
`to_canonical` and compared with `isobmff/ovt_min_structural.pybox.json`, written by the independent walker
`isobmff/gen_min_mp4.py` (`--check` verifies both files). It moves to `ovt probe` when the container parser exists.
