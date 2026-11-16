# WI-2.582 acceptance record

Machine: Linux x86_64 cloud container (4 vCPU), GCC 13, `linux-x64-debug` with the bundled patched libde265 1.0.15
(`OVT_SYSTEM_DE265=OFF`, `OVT_BUILD_PLUGIN_DE265=ON`) and the system FFmpeg 6.1 libraries, date 2026-11-16. The JCT-VC
conformance corpora are not reachable from cloud containers (itu.int is blocked), so the corpus tests below ran on four
stand-in x265 streams only (8-bit, 10-bit, WPP, a conformance window); the 30-stream runs are CI's job
(`tools/fetch_corpora.py --tags fast`). Spec version: 1.0-rc35. Questions and assumptions: issue #589.

## Test 1: grid fields and block tree equal HM

`ctest -R "h265_mv_vs_hm|h265_cu_tree_vs_hm|SAO per CTB"` in `ovt_plugin_de265_map_tests` (`test_de265_map.cpp`). The
grid test now runs every `jctvc` fast stream that has an `hm-grid` golden (30 streams; was 7), less
`SLIST_A_Sony_5` (libde265 mis-decodes it, #254) and `MVCLIP_A_qualcomm_3` (nightly tier, as before). Per 4x4 cell:
`pred, qp, intra_mode_y/c, mv0/1_x/y (after the ±32767 clamp), ref0/1, tx_log2_w, tx_log2_h (new), cbf` bit 0. The CU
tree (AMP_A) and the SAO parameters (SAO_A) compare against the two `hm-cutree` goldens as before.
Found on CI's first run and handled in the test (each is a plugin or libde265 behaviour, listed in #589): with no worker
threads a conformance window makes the plugin report the cropped picture, so `CONFWIN_A` runs with one thread; RPS_C and
CONFWIN_A raise mapping warnings with an empty Diagnostic code (V-029, SPEC-ASSUMPTION #261), which the plugin runs now
allow as the one validator finding (RPS_C and CONFWIN_A only). Not compared, issue #589: `cbf` Cb/Cr (patch 0002),
`partition_id` and the block
tree on the other streams (no HM data).
Local run: skipped (no corpus); compiled and linked clean under `-Werror`.

## Test 2: reconstruction equals the FFmpeg hevc software decode

`ctest -R de265_recon_vs_ffmpeg` (`ovt_plugin_de265_recon_tests`, `test_de265_recon_vs_ffmpeg.cpp`): the plugin's
`PlaneStage::Reconstruction` planes against libavcodec's `hevc` decoder (one thread, `apply_cropping = 0`), picture by
picture in output order, one MD5 per plane, plus 1 thread against 4 threads. Same stream list as test 1 plus AMP_A.
`NoOutPrior_A` is compared as a subsequence: libde265 outputs 10
pictures there that FFmpeg drops (no_output_of_prior_pics, #619); exactly 10 extra; every FFmpeg picture must
still
equal a plugin picture in order. All other streams must give the same sequence.

```text
OVT_CORPORA_DIR=<stand-ins> ovt_plugin_de265_recon_tests
test cases: 2 | 0 passed | 2 skipped (MVCLIP_A, no stand-in)  -- the generator case passed: assertions: 3620 | 3620
passed
```

(4 stand-in streams: 208x120, 416x240 B-pyramid, 10-bit WPP 192x144, 200x110 with a conformance window.)

## Test 3: `bench_analysis_de265_gop60_1080p`

Not done in this PR. The budget row exists in `tests/perf/budgets.json`, but the benchmark harness (WI-0.560b) is not on
`main` and `m1-air-8gb` is a manual run. See #589 item 5.

## Fuzz target

`fuzz_plugin_push_de265` (`plugins/ovt-plugin-de265/tests/fuzz/`), seeds in `core/tests/fuzz/corpus/plugin_push_de265/`:
`ctest -R fuzz_plugin_push_de265_regress` passes; 600 randomly mutated seeds replayed without a crash or contract
violation. The 60 s libFuzzer run belongs to CI (`OVT_ENABLE_FUZZ`).

`RAP_B_Bossen_2` is not in the recon list: on CI FFmpeg's output picture 69 of 80 equals no libde265
picture (the stream has end-of-sequence CRAs with RASL pictures). Which decoder is wrong is open (#620); its HM
grid comparison passes.
