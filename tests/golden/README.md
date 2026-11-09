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

## H.264/HEVC oracle goldens (WI-0.567)

`h264/`, `h265/` and `epb/` are written by `tools/goldens/gen_h26x.py` in the oracle image (workflow `goldens-h26x`), never
by hand. They are stored **gzip-compressed** (`<id>.<tool>.json.gz`, no name or time in the gzip header, so the bytes are
reproducible): the repository keeps every version for ever, and the plain JSON of 250 goldens is about 38 MB. A test
reads them with `gzip.open` / zlib; the canonical JSON inside is exactly what §14.4.0 describes. Each golden covers the first
pictures of its stream (`h26x-index.json` records how many, the SHA-256 of the corpus file used, and every skipped oracle
with the reason). `KNOWN_DIFFS.md` lists documented disagreements between `trace_headers` and `gstparse`.

## Metric oracle goldens (WI-0.565)

`metrics/` is written by `tools/goldens/gen_metrics.py` in the oracle image (workflow `goldens-metrics`), never by hand.
Each pair is the reference and the distorted Y4M file cut to its first N pictures (48 for the Netflix pair, 60 for the
two 1080p `syn_metrics_*` pairs; `metrics-index.json` records N, the SHA-256 of the cut inputs, the oracle versions and
every skipped step). Files, all `scores` JSON, gzip-compressed like the WI-0.567 goldens:

| File | Oracle | Spec |
|---|---|---|
| `<pair>.ffpsnr.json.gz` | FFmpeg `psnr` filter (values printed with `%0.2f`) | §14.4.10 |
| `<pair>.ffvmaf.json.gz` | FFmpeg `libvmaf` filter, `vmaf_v0.6.1` + `psnr`, `float_ssim` | §14.4.10 |
| `<pair>.<model>.vmaf.json.gz` | `vmaf` CLI, every bundled model, features `psnr float_ssim float_ms_ssim psnr_hvs ciede cambi`; `<pair>.vmaf_v0.6.1-sub5.vmaf.json.gz` is `--subsample 5` | §14.4.11 |
| `<pair>.x264ssim.json.gz` | `x264_ssim` (`tests/oracles/x264_ssim`: x264's own SSIM kernel, built from the repository by the generator) | §14.4.12 |
| `<pair>.psnrhvs.json.gz`, `<pair>.ciede.json.gz` | Daala `dump_psnrhvs -y`, `dump_ciede2000` (8-bit pairs; skipped for the 10-bit pair when the tools refuse it) | §14.4.13 |
| `bd/<set>.bd.json` | `bjontegaard` for every `rd_points/<set>.json` (`jvet4_*`: four points, `cubic` included; `aom6_*`: six points, no `cubic`; `*_partial`: partly overlapping curves) | §14.4.13 |

The pairs are `netflix_src01_576x324`, `syn_metrics_1080p` and `syn_metrics_pq_1080p` (10-bit). The generator fails when
the pooled `vmaf_v0.6.1` mean of the Netflix pair is not Netflix's published 76.6689 (acceptance test 1) or when
`x264_ssim` on the x264 reconstruction of the Netflix reference differs from the `SSIM Mean Y` of `x264 --ssim` by more
than 1e-4 (acceptance test 3). Acceptance test 2 (determinism) is the workflow's second run: both runs must be
byte-identical. UVQ goldens are WI-0.565b (below). Spec questions: #306.

## UVQ goldens (WI-0.565b)

`metrics/<clip>.uvq-<ver>.json.gz` (`ver` is `1.5` or `1.0`) for the `ytugc` `fast` clips `ytugc_clip01` to `03`, written by
`tools/goldens/gen_uvq.py` in the oracle image (workflow `goldens-uvq`), never by hand. The oracle is google/uvq (PyTorch,
the revision pinned in `tests/oracles/Dockerfile`) as `uvq_inference.py --model_version <ver> --device cpu --threads 1
--fps 1`, normalised by `tests/oracles/normalize/uvq.py` (`scores`, gzip like the other metric goldens). UVQ 1.5 has one
`frames` entry per sampled frame (`i` is the source frame index) and `pooled.uvq.mean`; UVQ 1.0 has no per-frame values
and keeps its sub-network scores in `pooled.uvq`. `uvq-index.json` records the clip hashes and the oracle revision. The
workflow runs the generator twice and the outputs must be byte-identical (acceptance test 2). Spec questions: #306.
