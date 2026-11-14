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

## Test 3 and step 5: passed on the Mac Mini (2026-11-13)

Machine: the project's Mac Mini (macOS, `ovt-build` account), YUView 2.14 from the official release installed under
`~/Applications`. YUView 2.14 has no `--loadfile` or `--quit-after-load` option and no exit code: files are plain
arguments and the application stays open, so the AT3 line of WI-0.089 was reworded accordingly (spec 1.0-rc24).

```sh
cp core/tests/golden/tiny16.yuv /tmp/tiny16_16x16.yuv        # the size in the name lets YUView infer 16x16
open -a YUView /tmp/tiny16_16x16.yuv core/tests/golden/tiny16_hevc.csv
```

Result: both items load with no error dialog (the only dialog was YUView's "newer version available" notice);
the statistics panel lists the OVT types from `OVT_Skip` through `OVT_DeblockBsH` without scrolling (the CSV
declares all 23 types, `OVT_PredMode` … `OVT_BlockGrid_TU`; the panel cannot be scrolled far enough to show all 23
on one screen). The application was closed by hand after about 20 seconds. Screenshot: attached to the Mac Mini
thread's report of 2026-11-13 04:03 UTC. No keyword was rejected, so the exporter needed no change.
