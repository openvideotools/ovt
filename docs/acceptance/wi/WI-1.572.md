# WI-1.572 acceptance record

The 10 `syn_h264_*` golden tests of part a use generated corpora (tests/corpora/synthetic/recipes.json). Hosted CI
does not generate them, so they exit 77 (skipped) there. Below is the Mac Mini run of PR #604 (00a6009): first the
container-path stream alone (the first time `h264_container_to_annexb` ran on the real file), then all 10 tests.

```console
WI-1.572a (PR #604 @ 00a6009) — syn_h264_multislice_cavlc_mp4 golden test on the Mac Mini, 2026-11-16 13:17 UTC
Machine: Mac16,10 Apple M4, macOS 15.8.2; preset macos-arm64-debug
Corpus: python3 -I tests/corpora/synthetic/generate.py --only syn_h264_multislice_cavlc_mp4 --out <dir> --check, in the ovt-oracles image (pinned encoders); hashes:
c6fe0640a2cb7d7561a40671b5e6334a9360602eeb267e86b4437361ca02e21d  syn_h264_multislice_cavlc
670a81ee31d36199d2d994e2d34a32789e71b7ca51330706d1b4ed4a997df72d  syn_h264_multislice_cavlc_mp4

$ OVT_CORPORA_DIR=<dir> OVT_REQUIRE_CORPORA=1 ctest --preset macos-arm64-debug -R "syntax_golden_h264_.*syn_h264_multislice_cavlc_mp4" --output-on-failure
Test project /Users/ovt-build/mxf-main/build/macos-arm64-debug
    Start 111: syntax_golden_h264_trace_headers_syn_h264_multislice_cavlc_mp4
    Start 137: syntax_golden_h264_gstparse_syn_h264_multislice_cavlc_mp4
1/2 Test #137: syntax_golden_h264_gstparse_syn_h264_multislice_cavlc_mp4 ........   Passed    0.22 sec
2/2 Test #111: syntax_golden_h264_trace_headers_syn_h264_multislice_cavlc_mp4 ...   Passed    0.22 sec

100% tests passed out of 2

Label Time Summary:
fast             =   0.44 sec*proc (2 tests)
golden           =   0.44 sec*proc (2 tests)
needs_corpus     =   0.44 sec*proc (2 tests)
syntax_golden    =   0.44 sec*proc (2 tests)

Total Test time (real) =   0.34 sec
rc=0

==================================================================
Full set, 2026-11-16 13:41 UTC — all 11 syn_h264 corpora generated in the ovt-oracles image with generate.py (each .ok sha256 matches tests/corpora/manifest.json; syn_h264_gop60_1080p with --check)
$ OVT_CORPORA_DIR=<dir> OVT_REQUIRE_CORPORA=1 ctest --preset macos-arm64-debug -R 'syntax_golden_h264_.*syn_h264' --output-on-failure
Test project /Users/ovt-build/mxf-main/build/macos-arm64-debug
      Start 111: syntax_golden_h264_trace_headers_syn_h264_multislice_cavlc_mp4
      Start 137: syntax_golden_h264_gstparse_syn_h264_multislice_cavlc_mp4
      Start 107: syntax_golden_h264_trace_headers_syn_h264_epb_dense
      Start 108: syntax_golden_h264_trace_headers_syn_h264_gop60_1080p
 1/10 Test #107: syntax_golden_h264_trace_headers_syn_h264_epb_dense ..............   Passed    0.06 sec
      Start 109: syntax_golden_h264_trace_headers_syn_h264_hrd_cbr
 2/10 Test #111: syntax_golden_h264_trace_headers_syn_h264_multislice_cavlc_mp4 ...   Passed    0.09 sec
      Start 110: syntax_golden_h264_trace_headers_syn_h264_hrd_vbr
 3/10 Test #137: syntax_golden_h264_gstparse_syn_h264_multislice_cavlc_mp4 ........   Passed    0.09 sec
      Start 133: syntax_golden_h264_gstparse_syn_h264_epb_dense
 4/10 Test #109: syntax_golden_h264_trace_headers_syn_h264_hrd_cbr ................   Passed    0.07 sec
      Start 134: syntax_golden_h264_gstparse_syn_h264_gop60_1080p
 5/10 Test #133: syntax_golden_h264_gstparse_syn_h264_epb_dense ...................   Passed    0.04 sec
      Start 135: syntax_golden_h264_gstparse_syn_h264_hrd_cbr
 6/10 Test #108: syntax_golden_h264_trace_headers_syn_h264_gop60_1080p ............   Passed    0.15 sec
      Start 136: syntax_golden_h264_gstparse_syn_h264_hrd_vbr
 7/10 Test #110: syntax_golden_h264_trace_headers_syn_h264_hrd_vbr ................   Passed    0.12 sec
 8/10 Test #135: syntax_golden_h264_gstparse_syn_h264_hrd_cbr .....................   Passed    0.08 sec
 9/10 Test #134: syntax_golden_h264_gstparse_syn_h264_gop60_1080p .................   Passed    0.15 sec
10/10 Test #136: syntax_golden_h264_gstparse_syn_h264_hrd_vbr .....................   Passed    0.12 sec

100% tests passed out of 10

Label Time Summary:
fast             =   0.97 sec*proc (10 tests)
golden           =   0.97 sec*proc (10 tests)
needs_corpus     =   0.97 sec*proc (10 tests)
syntax_golden    =   0.97 sec*proc (10 tests)

Total Test time (real) =   0.37 sec
rc=0
```

## Part b (#665): Mini run of the 36 synthetic HEVC tests

- Command: `OVT_REQUIRE_CORPORA=1 ctest --preset macos-arm64-debug -R 'syntax_golden_h265_.*syn_' --output-on-failure`
- Machine: Mac Mini, Apple M4, macOS 15.8.2. Corpora generated with
  `tests/corpora/synthetic/generate.py --only <id> --check` in the oracle image.
- Commit: 2ad6d97 (after #781 made the x265 streams reproducible). Date: 2026-11-28 (20261128T195226Z).
- Result: 36 of 36 pass. All 13 `syn_h265_*` streams match the manifest. The 5 `syn_pat_*` streams differ from
  their manifest hashes (a known corpus issue outside this WI, see #780); their header goldens pass.
- An earlier run on 9118052 (2026-11-21) gave 25 of 36: the 11 failures came from x265's host-dependent info SEI,
  fixed by #781.

```text
WI-1.572 (#665) syn H.265 syntax goldens, 2026-11-28 19:52 UTC.
Machine: Mac Mini, Apple M4 (arm64), macOS 15.8.2. Branch claude/project-thread-qejsqt at 2ad6d97 (merge of origin/main), macos-arm64-debug. Build rc=0. Nothing installed or pushed.

RESULT: 36 of 36 PASS (OVT_REQUIRE_CORPORA=1 ctest --preset macos-arm64-debug -R 'syntax_golden_h265_.*syn_' --output-on-failure).

generate.py --only <id> --check (local oracle image ovt-oracles:local, linux/amd64 under Rosetta):
  13 syn_h265_*: rc=0, hashes match the manifest (the info=0 re-pin from #780/#781 is in this merge).
  5 syn_pat_*: rc=5, HASH MISMATCH against the manifest, the same produced hashes as on 2026-11-21. Files are written anyway, and their 10
  tests pass. tools/gen_hdr_patterns.py output on this image differs from the manifest's; I haven't investigated why.
    == syn_h265_amp_sao_on rc=0
    == syn_h265_bpyramid rc=0
    == syn_h265_crop_1080_in_1088 rc=0
    == syn_h265_gop60_1080p rc=0
    == syn_h265_hdr10plus rc=0
    == syn_h265_hrd_cbr rc=0
    == syn_h265_hrd_vbr rc=0
    == syn_h265_main10_hev1 rc=0
    == syn_h265_main10_pq_hdr rc=0
    == syn_h265_multi_pps rc=0
    == syn_h265_pq_colr_only rc=0
    == syn_h265_pq_sei_only rc=0
    == syn_h265_sei_unknown_200 rc=0
    hash mismatch against the manifest:
      syn_pat_hlg_patches: manifest fd4b89443d17f59e75f841662a52a13813d7785ec497e9b4f9693afd50114e83, produced f5805d12ba9d7bdf8919ba557205e206bbc9d5bfd0f537cbd997657227521586
    == syn_pat_hlg_patches rc=5
    hash mismatch against the manifest:
      syn_pat_p3_saturation: manifest 733d0f2f45de8cdd4151e556d0fd8b90dabb9fdd3dba32aea12217e3aea6ed25, produced 880c4b78e13f444af12caff39185d8ca350be0235d74819535ba39839b5c8a30
    == syn_pat_p3_saturation rc=5
    hash mismatch against the manifest:
      syn_pat_pq_patches_1000: manifest 643f43daee0d52a789771a1c7c1251447c8f29411e5dbecad020ba86e0a4d6d9, produced 87ec595a7b25b9a42da6f6412c66901a61b0a47bcd1095e01531181fc53524e5
    == syn_pat_pq_patches_1000 rc=5
    hash mismatch against the manifest:
      syn_pat_sdr709_ramp: manifest 5097e4b82e3504aecbcd45d87f51ab748b0aed0f70a764e8708f0651c1b0dad7, produced 8e5bff6b76d7656cc0eccc24aa0ea57b3efccc7b2b8ff400772f98074fdfde6f
    == syn_pat_sdr709_ramp rc=5
    hash mismatch against the manifest:
      syn_pat_siting_checker: manifest 3da1c685209c677df12c4561d8a20aceefdc4c3e848f216888471f62633e2340, produced 888a306b6ada738a1c069bf809715f7659aa1fab79bce56cc701111c7264eb59
    == syn_pat_siting_checker rc=5

=== ctest output
Test project /Users/ovt-build/src/ovt/build/macos-arm64-debug
      Start 1612: syntax_golden_h265_gstparse_syn_h265_gop60_1080p
      Start 1615: syntax_golden_h265_gstparse_syn_h265_hrd_vbr
      Start 1549: syntax_golden_h265_trace_headers_syn_h265_crop_1080_in_1088
      Start 1614: syntax_golden_h265_gstparse_syn_h265_hrd_cbr
 1/36 Test #1549: syntax_golden_h265_trace_headers_syn_h265_crop_1080_in_1088 ...   Passed    0.18 sec
      Start 1616: syntax_golden_h265_gstparse_syn_h265_main10_hev1
 2/36 Test #1614: syntax_golden_h265_gstparse_syn_h265_hrd_cbr ..................   Passed    0.24 sec
      Start 1548: syntax_golden_h265_trace_headers_syn_h265_bpyramid
 3/36 Test #1548: syntax_golden_h265_trace_headers_syn_h265_bpyramid ............   Passed    0.03 sec
      Start 1617: syntax_golden_h265_gstparse_syn_h265_main10_pq_hdr
 4/36 Test #1616: syntax_golden_h265_gstparse_syn_h265_main10_hev1 ..............   Passed    0.08 sec
      Start 1621: syntax_golden_h265_gstparse_syn_h265_sei_unknown_200
 5/36 Test #1615: syntax_golden_h265_gstparse_syn_h265_hrd_vbr ..................   Passed    0.29 sec
      Start 1613: syntax_golden_h265_gstparse_syn_h265_hdr10plus
 6/36 Test #1612: syntax_golden_h265_gstparse_syn_h265_gop60_1080p ..............   Passed    0.29 sec
      Start 1618: syntax_golden_h265_gstparse_syn_h265_multi_pps
 7/36 Test #1621: syntax_golden_h265_gstparse_syn_h265_sei_unknown_200 ..........   Passed    0.05 sec
      Start 1623: syntax_golden_h265_gstparse_syn_pat_p3_saturation
 8/36 Test #1617: syntax_golden_h265_gstparse_syn_h265_main10_pq_hdr ............   Passed    0.08 sec
      Start 1619: syntax_golden_h265_gstparse_syn_h265_pq_colr_only
 9/36 Test #1618: syntax_golden_h265_gstparse_syn_h265_multi_pps ................   Passed    0.05 sec
      Start 1622: syntax_golden_h265_gstparse_syn_pat_hlg_patches
10/36 Test #1623: syntax_golden_h265_gstparse_syn_pat_p3_saturation .............   Passed    0.04 sec
      Start 1625: syntax_golden_h265_gstparse_syn_pat_sdr709_ramp
11/36 Test #1613: syntax_golden_h265_gstparse_syn_h265_hdr10plus ................   Passed    0.08 sec
      Start 1624: syntax_golden_h265_gstparse_syn_pat_pq_patches_1000
12/36 Test #1622: syntax_golden_h265_gstparse_syn_pat_hlg_patches ...............   Passed    0.05 sec
      Start 1620: syntax_golden_h265_gstparse_syn_h265_pq_sei_only
13/36 Test #1619: syntax_golden_h265_gstparse_syn_h265_pq_colr_only .............   Passed    0.06 sec
      Start 1611: syntax_golden_h265_gstparse_syn_h265_crop_1080_in_1088
14/36 Test #1625: syntax_golden_h265_gstparse_syn_pat_sdr709_ramp ...............   Passed    0.05 sec
      Start 1609: syntax_golden_h265_gstparse_syn_h265_amp_sao_on
15/36 Test #1624: syntax_golden_h265_gstparse_syn_pat_pq_patches_1000 ...........   Passed    0.05 sec
      Start 1626: syntax_golden_h265_gstparse_syn_pat_siting_checker
16/36 Test #1620: syntax_golden_h265_gstparse_syn_h265_pq_sei_only ..............   Passed    0.06 sec
      Start 1560: syntax_golden_h265_trace_headers_syn_pat_hlg_patches
17/36 Test #1611: syntax_golden_h265_gstparse_syn_h265_crop_1080_in_1088 ........   Passed    0.05 sec
      Start 1610: syntax_golden_h265_gstparse_syn_h265_bpyramid
18/36 Test #1609: syntax_golden_h265_gstparse_syn_h265_amp_sao_on ...............   Passed    0.06 sec
      Start 1561: syntax_golden_h265_trace_headers_syn_pat_p3_saturation
19/36 Test #1626: syntax_golden_h265_gstparse_syn_pat_siting_checker ............   Passed    0.05 sec
      Start 1563: syntax_golden_h265_trace_headers_syn_pat_sdr709_ramp
20/36 Test #1560: syntax_golden_h265_trace_headers_syn_pat_hlg_patches ..........   Passed    0.03 sec
      Start 1562: syntax_golden_h265_trace_headers_syn_pat_pq_patches_1000
21/36 Test #1561: syntax_golden_h265_trace_headers_syn_pat_p3_saturation ........   Passed    0.03 sec
      Start 1564: syntax_golden_h265_trace_headers_syn_pat_siting_checker
22/36 Test #1610: syntax_golden_h265_gstparse_syn_h265_bpyramid .................   Passed    0.05 sec
      Start 1547: syntax_golden_h265_trace_headers_syn_h265_amp_sao_on
23/36 Test #1563: syntax_golden_h265_trace_headers_syn_pat_sdr709_ramp ..........   Passed    0.03 sec
      Start 1550: syntax_golden_h265_trace_headers_syn_h265_gop60_1080p
24/36 Test #1562: syntax_golden_h265_trace_headers_syn_pat_pq_patches_1000 ......   Passed    0.03 sec
      Start 1551: syntax_golden_h265_trace_headers_syn_h265_hdr10plus
25/36 Test #1564: syntax_golden_h265_trace_headers_syn_pat_siting_checker .......   Passed    0.04 sec
      Start 1552: syntax_golden_h265_trace_headers_syn_h265_hrd_cbr
26/36 Test #1547: syntax_golden_h265_trace_headers_syn_h265_amp_sao_on ..........   Passed    0.04 sec
      Start 1553: syntax_golden_h265_trace_headers_syn_h265_hrd_vbr
27/36 Test #1551: syntax_golden_h265_trace_headers_syn_h265_hdr10plus ...........   Passed    0.07 sec
      Start 1554: syntax_golden_h265_trace_headers_syn_h265_main10_hev1
28/36 Test #1552: syntax_golden_h265_trace_headers_syn_h265_hrd_cbr .............   Passed    0.08 sec
      Start 1555: syntax_golden_h265_trace_headers_syn_h265_main10_pq_hdr
29/36 Test #1550: syntax_golden_h265_trace_headers_syn_h265_gop60_1080p .........   Passed    0.12 sec
      Start 1556: syntax_golden_h265_trace_headers_syn_h265_multi_pps
30/36 Test #1554: syntax_golden_h265_trace_headers_syn_h265_main10_hev1 .........   Passed    0.06 sec
      Start 1557: syntax_golden_h265_trace_headers_syn_h265_pq_colr_only
31/36 Test #1556: syntax_golden_h265_trace_headers_syn_h265_multi_pps ...........   Passed    0.04 sec
      Start 1558: syntax_golden_h265_trace_headers_syn_h265_pq_sei_only
32/36 Test #1553: syntax_golden_h265_trace_headers_syn_h265_hrd_vbr .............   Passed    0.12 sec
      Start 1559: syntax_golden_h265_trace_headers_syn_h265_sei_unknown_200
33/36 Test #1555: syntax_golden_h265_trace_headers_syn_h265_main10_pq_hdr .......   Passed    0.05 sec
34/36 Test #1557: syntax_golden_h265_trace_headers_syn_h265_pq_colr_only ........   Passed    0.04 sec
35/36 Test #1559: syntax_golden_h265_trace_headers_syn_h265_sei_unknown_200 .....   Passed    0.03 sec
36/36 Test #1558: syntax_golden_h265_trace_headers_syn_h265_pq_sei_only .........   Passed    0.04 sec

100% tests passed out of 36

Label Time Summary:
fast             =   2.73 sec*proc (36 tests)
golden           =   2.73 sec*proc (36 tests)
needs_corpus     =   2.73 sec*proc (36 tests)
syntax_golden    =   2.73 sec*proc (36 tests)

Total Test time (real) =   2.87 sec
```
