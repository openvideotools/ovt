# WI-2.581 acceptance record

Machine: Linux x86_64 cloud container (4 vCPU), GCC 13, `linux-x64-debug` configuration with the bundled libaom 3.12.1
(`OVT_SYSTEM_LIBAOM=OFF`, `OVT_BUILD_PLUGIN_AOM=ON`), date 2026-11-14. Corpus: the 21 `aom_av1_1_*` entries of
`tests/golden/av1-index.json`, fetched with `tools/fetch_corpora.py`. The `syn_av1_*` streams need the pinned `aomenc`
and are exercised by CI only (the test skips an entry whose corpus file is not fetched, and fails under
`OVT_REQUIRE_CORPORA=1`). Spec version: 1.0-rc24. Questions and assumptions: issue #499.

## Test 1: grid fields, `symbols` and `bits_x8` equal the golden

```text
OVT_CORPORA_DIR=~/.cache/ovt/corpora ctest -R golden_av1_inspect -j6
100% tests passed, 0 tests failed out of 7        (AT1 groups 0..5 and AT2)
```

`golden_av1_inspect AT1 group 0..5` run every golden of `tests/golden/av1` (six groups of ids). For each frame the plugin's
`FrameAnalysis` is converted to the golden's shape and compared: header (`base_q_idx`, `delta_q_*`, `frame_type`,
`show_frame`, tile size), every grid layer cell by cell (25 layers), every `(x4, y4, name)` symbol total, and for every
leaf block `Block.bits_x8` against the sum of the oracle's symbols in the cells it owns. Not compared, with the reason in
issue 499: `TxType` (no tx_type vector until WI-2.159), the header's `frame_number` (the host fills `FrameInfo.poc`), the
CDEF strength layers except on blocks that cover a known unit origin, and the inter-only layers on intra blocks (libaom
prints stale memory there). The committed goldens lack the first accounting symbol of each context (stock
`inspect`, issue 103): the test adds those records back from libaom's own accounting (`kGoldensHaveFirstSymbol`).
Streams not read by the test: Annex B and container-wrapped files (`aom_av1_1_b8_03_size{up,down}.mkv`, `syn_av1_annexb*`).

Sensitivity check (not kept): adding 1 to the plugin's `qp` of one cell, and setting `kGoldensHaveFirstSymbol = true`, each
turn the test red.

## Test 2: threads = 1 and threads = 4

`golden_av1_inspect AT2` analyses `aom_av1_1_b8_01_size_66x66`, `aom_av1_1_b8_05_mv`, `aom_av1_1_b8_22_svc_L1T2`,
`aom_av1_1_b8_22_svc_L2T1` (and `syn_av1_tiles_cdef_lr`, `syn_av1_hidden_altref_showexisting` when fetched) with 1 and 4
threads and requires identical `FrameAnalysis` bytes: passed.

## Test 3: `bench_analysis_aom_gop60_1080p`

Not done in this PR. The budget row exists in `tests/perf/budgets.json`, but the benchmark harness binaries (WI-0.560b) are
not on `main`, and the `m1-air-8gb` device is a manual run. See #499 item 5.

## Fuzz target

`fuzz_plugin_push_aom` (`plugins/ovt-plugin-aom/tests/fuzz/`), seeds in `core/tests/fuzz/corpus/plugin_push_aom/`:
`ctest -R fuzz_plugin_push_aom_regress` passes; 600 randomly mutated seeds replayed without a crash or contract violation.
The 60 s libFuzzer run belongs to CI (`OVT_ENABLE_FUZZ`).
