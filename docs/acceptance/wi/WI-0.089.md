# WI-0.089 acceptance record

WI-0.089 is split (#233, §16.2.6): part a (this record) is the exporter, part b the importer with acceptance
test 2 and the fuzz target. Part b appends its results here.

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, clang 18.1, CMake 3.28.3, Ninja, system
flatbuffers 2.0.8 (vcpkg and third-party downloads are blocked in this container).
Date: 2026-11-07. Spec version: 1.0-rc14. Commit: the head of the WI-0.089 pull request, on top of WI-0.093a
(#165), which provides `core/tests/golden/tiny16_hevc.fa` and `tiny16.yuv`.

## Test 1: automated (part a)

| # | Given / When / Then | Test |
|---|---|---|
| 1 | `tiny16_hevc.fa` exported with `write_yuview_csv` equals `core/tests/golden/tiny16_hevc.csv` | `yuview_csv: tiny16_hevc.fa exports to the golden CSV (WI-0.089 AT1)` in `test_yuview_csv`, which also exports twice and compares the bytes (§0.7.5) |

The golden CSV was written by the exporter and then checked by hand against §6.8.2: the header, the PredMode
`mapColor` table, `defaultRange;0;51;jet`, the IntraModeY colours 0/1/26, the CU grid rows and the example's
data rows all match. The one difference is the example's `0;8;8;8;8;1;34`: the fixture's QpY there is 33 (#233
item 3).

```console
$ ctest --preset pr
100% tests passed, 0 tests failed out of 251
$ ctest --preset linux-x64-debug -L golden
100% tests passed, 0 tests failed out of 30
```

## Test 2: part b (importer)

Pending: WI-0.089b.

## Test 3 and step 5: pending (#233 item 5)

AT3 (YUView 2.14 loads `tiny16.yuv` plus `tiny16_hevc.csv` and exits 0 with no "error" line) and the step 5
screenshot need a YUView binary. This container can't download one. To run them on a desktop with YUView 2.14:

```sh
YUView --loadfile core/tests/golden/tiny16.yuv --loadfile core/tests/golden/tiny16_hevc.csv --quit-after-load
```

Set the YUV item to 16×16 YUV 4:2:0 8-bit. The statistics item should list 23 OVT types (PredMode … BlockGrid_TU)
over one frame. Record the command, the exit code, the log and a screenshot here. If YUView rejects a
keyword, the exporter gets fixed, not the spec (§6.8.2 verification note).
