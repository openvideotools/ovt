# WI-0.089 acceptance record

WI-0.089 was split for the 1 500-line limit (§16.2.6): WI-0.089a (the exporter and AT1, #234, merged as
5fa4d2e) and WI-0.089b (the importer, AT2, the fuzz target and the sample files).

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, clang 18.1, CMake 3.28.3, Ninja, system
flatbuffers 2.0.8 (vcpkg and third-party downloads are blocked in this container).
Date: 2026-11-08. Spec version: 1.0-rc14. Commit: the head of the WI-0.089b pull request, on top of main 5fa4d2e.

## Tests 1 and 2: automated

| # | Given / When / Then | Test |
|---|---|---|
| 1 | `tiny16_hevc.fa` exported with `write_yuview_csv` equals `core/tests/golden/tiny16_hevc.csv` | `yuview_csv: tiny16_hevc.fa exports to the golden CSV (WI-0.089 AT1)` in `test_yuview_csv`, which also exports twice and compares the bytes (§0.7.5) |
| 2 | The golden CSV, imported with `read_yuview_csv`, gives `grid.qp` and `grid.intra_mode_y` equal to the fixture's | `yuview_csv: the golden CSV imports back to the fixture's grid (WI-0.089 AT2)` |

The golden CSV was written by the exporter and then checked by hand against §6.8.2: the header, the PredMode
`mapColor` table, `defaultRange;0;51;jet`, the IntraModeY colours 0/1/26, the CU grid rows and the example's
data rows all match. The one difference is the example's `0;8;8;8;8;1;34`: the fixture's QpY there is 33 (#233
item 3).

The importer is also tested on three hand-written CC0 files in `core/tests/data/yuview/` (foreign type names,
several POCs with vectors and CRLF line ends, malformed lines), in `yuview_csv: importer reads the hand-written
sample files`. They replace YUView's own GPL-3.0 samples (#233 item 4, coordinator's decision).

```console
$ build/linux-x64-debug/core/tests/model/test_yuview_csv
All tests passed (196 assertions in 7 test cases)
$ ctest --preset linux-x64-debug -L golden
100% tests passed, 0 tests failed out of 24
$ ctest --preset pr
99% tests passed, 3 tests failed out of 314
```

The three `pr` failures (`cli_completion_zsh`, `cli_no_args`, `downstream_consumer`) are local only: this
container's system `libflatbuffers.a` is not built with `-fPIC`, so `libovt_core.so` does not link. They do
not touch this WI; CI builds flatbuffers through vcpkg.

Fuzz smoke of the importer (Definition of Done 3), built with clang 18 and `-fsanitize=fuzzer,address,undefined`
from `core/tests/fuzz/fuzz_yuview_csv.cpp`, seeded with `core/tests/fuzz/corpus/yuview_csv/` and the three
sample files:

```console
$ fuzz_yuview -max_total_time=60 -timeout=1 -rss_limit_mb=2048 corpus/
Done 253592 runs in 61 second(s)
```

No crash, timeout, leak or sanitizer report.

## Test 3 and step 5: pending (#233 item 5)

AT3 (YUView 2.14 loads `tiny16.yuv` plus `tiny16_hevc.csv` and exits 0 with no "error" line) and the step 5
screenshot need a YUView binary. This container can't download one. To run them on a desktop with YUView 2.14:

```sh
YUView --loadfile core/tests/golden/tiny16.yuv --loadfile core/tests/golden/tiny16_hevc.csv --quit-after-load
```

Set the YUV item to 16×16 YUV 4:2:0 8-bit. The statistics item should list 23 OVT types (PredMode … BlockGrid_TU)
over one frame. Record the command, the exit code, the log and a screenshot here. If YUView rejects a
keyword, the exporter gets fixed, not the spec (§6.8.2 verification note).
