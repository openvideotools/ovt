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
  `tests/corpora/synthetic/generate.py --check` in the oracle image.
- Commit: 9118052. Date: 2026-11-21 (20261121T231330Z).
- Result: 25 of 36 pass. The 11 failures (`syntax_golden_h265_trace_headers_syn_h265_*`) differ only in 5
  `user_data_payload_byte` values of x265's version-info SEI (`cpuid=`): the committed goldens were made from
  streams encoded on another CPU. Tracked in #780 (recipes need `info=0`, then re-pin the manifest and regenerate
  the goldens); this record is redone after #780 lands.

```text
WI-1.572b (#665) syn H.265 syntax goldens, 20261121T231330Z. Mac Mini, Apple M4, macOS 15.8.2. Branch claude/project-thread-qejsqt at 9118052, macos-arm64-debug. Nothing pushed.

RESULT: 25 of 36 pass, 11 fail. Every failure is the same 5 bytes of x265's version-info SEI. That's an environment difference, not a parser bug.
  The failing tests are syntax_golden_h265_trace_headers_syn_h265_{amp_sao_on, hdr10plus, gop60_1080p, hrd_cbr, hrd_vbr, main10_hev1, main10_pq_hdr,
  multi_pps, pq_sei_only, pq_colr_only, sei_unknown_200}. In each, the golden and OVT element counts are equal, and the only diffs (55 total, 5 per
  stream) are user_data_payload_byte inside the x265 user_data_unregistered SEI. The SEI text embeds the encoding host's CPU flags:
  this stream says 'x265 (build 215) - 4.1:[Linux][GCC 13.3.0] ... options: cpuid=1049071 ...', and the golden's bytes read 'cpuid=1111039'
  at the same offsets (49,49,49,_,51,57 vs 48,52,57,_,55,49).
  Our streams match the manifest hashes ('generate.py --check' passed for all 13 syn_h265_*), so the committed goldens look like they were
  made from streams encoded on a different CPU than the manifest's. Suggest: have the h265 golden comparison ignore user_data_payload_byte of the
  x265 info SEI (UUID 2CA2DE09-B517-47DB-BB55-A4FE7F5C2E36), or regenerate the goldens from manifest-hash streams. A recipe change to pass
  --no-info would also remove the host dependence, but it changes the manifest.
  All 18 other comparisons pass, including the 5 syn_pat_* streams and the syn_h265_bpyramid / crop_1080_in_1088 trace_headers tests.

CORPORA: the 18 streams were generated in the local oracle image (ovt-oracles:local, linux/amd64 under Rosetta) with
  tests/corpora/synthetic/generate.py --only <id> --out ~/wi1572/corpora665 --check
  13 syn_h265_*: rc=0, hashes match the manifest.
  5 syn_pat_*: rc=5, HASH MISMATCH against the manifest (files written anyway), yet their tests pass:
    hash mismatch against the manifest:
      syn_pat_hlg_patches: manifest fd4b89443d17f59e75f841662a52a13813d7785ec497e9b4f9693afd50114e83, produced f5805d12ba9d7bdf8919ba557205e206bbc9d5bfd0f537cbd997657227521586
    hash mismatch against the manifest:
      syn_pat_p3_saturation: manifest 733d0f2f45de8cdd4151e556d0fd8b90dabb9fdd3dba32aea12217e3aea6ed25, produced 880c4b78e13f444af12caff39185d8ca350be0235d74819535ba39839b5c8a30
    hash mismatch against the manifest:
      syn_pat_pq_patches_1000: manifest 643f43daee0d52a789771a1c7c1251447c8f29411e5dbecad020ba86e0a4d6d9, produced 87ec595a7b25b9a42da6f6412c66901a61b0a47bcd1095e01531181fc53524e5
    hash mismatch against the manifest:
      syn_pat_sdr709_ramp: manifest 5097e4b82e3504aecbcd45d87f51ab748b0aed0f70a764e8708f0651c1b0dad7, produced 8e5bff6b76d7656cc0eccc24aa0ea57b3efccc7b2b8ff400772f98074fdfde6f
    hash mismatch against the manifest:
      syn_pat_siting_checker: manifest 3da1c685209c677df12c4561d8a20aceefdc4c3e848f216888471f62633e2340, produced 888a306b6ada738a1c069bf809715f7659aa1fab79bce56cc701111c7264eb59
    (tools/gen_hdr_patterns.py output differs from the manifest on this image. Possibly a numpy or ffmpeg build difference; not investigated.)

$ OVT_CORPORA_DIR=~/wi1572/corpora665 OVT_REQUIRE_CORPORA=1 ctest --preset macos-arm64-debug -R 'syntax_golden_h265_.*syn_' --output-on-failure
Test project /Users/ovt-build/src/ovt/build/macos-arm64-debug
      Start 1367: syntax_golden_h265_trace_headers_syn_h265_amp_sao_on
      Start 1368: syntax_golden_h265_trace_headers_syn_h265_bpyramid
      Start 1369: syntax_golden_h265_trace_headers_syn_h265_crop_1080_in_1088
      Start 1370: syntax_golden_h265_trace_headers_syn_h265_gop60_1080p
 1/36 Test #1368: syntax_golden_h265_trace_headers_syn_h265_bpyramid ............   Passed    0.19 sec
      Start 1371: syntax_golden_h265_trace_headers_syn_h265_hdr10plus
 2/36 Test #1369: syntax_golden_h265_trace_headers_syn_h265_crop_1080_in_1088 ...   Passed    0.20 sec
      Start 1372: syntax_golden_h265_trace_headers_syn_h265_hrd_cbr
 3/36 Test #1367: syntax_golden_h265_trace_headers_syn_h265_amp_sao_on ..........***Failed    0.24 sec
syn_h265_amp_sao_on.265: 10 units parsed, 2754 golden elements, 2754 OVT elements
unit 3: golden user_data_payload_byte [1576+8] = 49 | ovt user_data_payload_byte [1576+8] = 48
unit 3: golden user_data_payload_byte [1584+8] = 49 | ovt user_data_payload_byte [1584+8] = 52
unit 3: golden user_data_payload_byte [1592+8] = 49 | ovt user_data_payload_byte [1592+8] = 57
unit 3: golden user_data_payload_byte [1608+8] = 51 | ovt user_data_payload_byte [1608+8] = 55
unit 3: golden user_data_payload_byte [1616+8] = 57 | ovt user_data_payload_byte [1616+8] = 49

      Start 1373: syntax_golden_h265_trace_headers_syn_h265_hrd_vbr
 4/36 Test #1371: syntax_golden_h265_trace_headers_syn_h265_hdr10plus ...........***Failed    0.10 sec
syn_h265_hdr10plus.mp4: 21 units parsed, 3494 golden elements, 3494 OVT elements
unit 5: golden user_data_payload_byte [1488+8] = 49 | ovt user_data_payload_byte [1488+8] = 48
unit 5: golden user_data_payload_byte [1496+8] = 49 | ovt user_data_payload_byte [1496+8] = 52
unit 5: golden user_data_payload_byte [1504+8] = 49 | ovt user_data_payload_byte [1504+8] = 57
unit 5: golden user_data_payload_byte [1520+8] = 51 | ovt user_data_payload_byte [1520+8] = 55
unit 5: golden user_data_payload_byte [1528+8] = 57 | ovt user_data_payload_byte [1528+8] = 49

      Start 1374: syntax_golden_h265_trace_headers_syn_h265_main10_hev1
 5/36 Test #1370: syntax_golden_h265_trace_headers_syn_h265_gop60_1080p .........***Failed    0.33 sec
syn_h265_gop60_1080p.mp4: 13 units parsed, 2966 golden elements, 2966 OVT elements
unit 3: golden user_data_payload_byte [1576+8] = 49 | ovt user_data_payload_byte [1576+8] = 48
unit 3: golden user_data_payload_byte [1584+8] = 49 | ovt user_data_payload_byte [1584+8] = 52
unit 3: golden user_data_payload_byte [1592+8] = 49 | ovt user_data_payload_byte [1592+8] = 57
unit 3: golden user_data_payload_byte [1608+8] = 51 | ovt user_data_payload_byte [1608+8] = 55
unit 3: golden user_data_payload_byte [1616+8] = 57 | ovt user_data_payload_byte [1616+8] = 49

      Start 1375: syntax_golden_h265_trace_headers_syn_h265_main10_pq_hdr
 6/36 Test #1372: syntax_golden_h265_trace_headers_syn_h265_hrd_cbr .............***Failed    0.16 sec
syn_h265_hrd_cbr.265: 18 units parsed, 2993 golden elements, 2993 OVT elements
unit 3: golden user_data_payload_byte [1576+8] = 49 | ovt user_data_payload_byte [1576+8] = 48
unit 3: golden user_data_payload_byte [1584+8] = 49 | ovt user_data_payload_byte [1584+8] = 52
unit 3: golden user_data_payload_byte [1592+8] = 49 | ovt user_data_payload_byte [1592+8] = 57
unit 3: golden user_data_payload_byte [1608+8] = 51 | ovt user_data_payload_byte [1608+8] = 55
unit 3: golden user_data_payload_byte [1616+8] = 57 | ovt user_data_payload_byte [1616+8] = 49

      Start 1376: syntax_golden_h265_trace_headers_syn_h265_multi_pps
 7/36 Test #1374: syntax_golden_h265_trace_headers_syn_h265_main10_hev1 .........***Failed    0.20 sec
syn_h265_main10_hev1.mp4: 15 units parsed, 3074 golden elements, 3074 OVT elements
unit 5: golden user_data_payload_byte [1488+8] = 49 | ovt user_data_payload_byte [1488+8] = 48
unit 5: golden user_data_payload_byte [1496+8] = 49 | ovt user_data_payload_byte [1496+8] = 52
unit 5: golden user_data_payload_byte [1504+8] = 49 | ovt user_data_payload_byte [1504+8] = 57
unit 5: golden user_data_payload_byte [1520+8] = 51 | ovt user_data_payload_byte [1520+8] = 55
unit 5: golden user_data_payload_byte [1528+8] = 57 | ovt user_data_payload_byte [1528+8] = 49

      Start 1377: syntax_golden_h265_trace_headers_syn_h265_pq_colr_only
 8/36 Test #1375: syntax_golden_h265_trace_headers_syn_h265_main10_pq_hdr .......***Failed    0.17 sec
syn_h265_main10_pq_hdr.mp4: 15 units parsed, 3074 golden elements, 3074 OVT elements
unit 5: golden user_data_payload_byte [1488+8] = 49 | ovt user_data_payload_byte [1488+8] = 48
unit 5: golden user_data_payload_byte [1496+8] = 49 | ovt user_data_payload_byte [1496+8] = 52
unit 5: golden user_data_payload_byte [1504+8] = 49 | ovt user_data_payload_byte [1504+8] = 57
unit 5: golden user_data_payload_byte [1520+8] = 51 | ovt user_data_payload_byte [1520+8] = 55
unit 5: golden user_data_payload_byte [1528+8] = 57 | ovt user_data_payload_byte [1528+8] = 49

      Start 1378: syntax_golden_h265_trace_headers_syn_h265_pq_sei_only
 9/36 Test #1373: syntax_golden_h265_trace_headers_syn_h265_hrd_vbr .............***Failed    0.31 sec
syn_h265_hrd_vbr.265: 18 units parsed, 3016 golden elements, 3016 OVT elements
unit 3: golden user_data_payload_byte [1576+8] = 49 | ovt user_data_payload_byte [1576+8] = 48
unit 3: golden user_data_payload_byte [1584+8] = 49 | ovt user_data_payload_byte [1584+8] = 52
unit 3: golden user_data_payload_byte [1592+8] = 49 | ovt user_data_payload_byte [1592+8] = 57
unit 3: golden user_data_payload_byte [1608+8] = 51 | ovt user_data_payload_byte [1608+8] = 55
unit 3: golden user_data_payload_byte [1616+8] = 57 | ovt user_data_payload_byte [1616+8] = 49

      Start 1379: syntax_golden_h265_trace_headers_syn_h265_sei_unknown_200
10/36 Test #1376: syntax_golden_h265_trace_headers_syn_h265_multi_pps ...........***Failed    0.21 sec
syn_h265_multi_pps.265: 12 units parsed, 2844 golden elements, 2844 OVT elements
unit 5: golden user_data_payload_byte [1576+8] = 49 | ovt user_data_payload_byte [1576+8] = 48
unit 5: golden user_data_payload_byte [1584+8] = 49 | ovt user_data_payload_byte [1584+8] = 52
unit 5: golden user_data_payload_byte [1592+8] = 49 | ovt user_data_payload_byte [1592+8] = 57
unit 5: golden user_data_payload_byte [1608+8] = 51 | ovt user_data_payload_byte [1608+8] = 55
unit 5: golden user_data_payload_byte [1616+8] = 57 | ovt user_data_payload_byte [1616+8] = 49

      Start 1380: syntax_golden_h265_trace_headers_syn_pat_hlg_patches
11/36 Test #1378: syntax_golden_h265_trace_headers_syn_h265_pq_sei_only .........***Failed    0.17 sec
syn_h265_pq_sei_only.265: 12 units parsed, 2878 golden elements, 2878 OVT elements
unit 5: golden user_data_payload_byte [1488+8] = 49 | ovt user_data_payload_byte [1488+8] = 48
unit 5: golden user_data_payload_byte [1496+8] = 49 | ovt user_data_payload_byte [1496+8] = 52
unit 5: golden user_data_payload_byte [1504+8] = 49 | ovt user_data_payload_byte [1504+8] = 57
unit 5: golden user_data_payload_byte [1520+8] = 51 | ovt user_data_payload_byte [1520+8] = 55
unit 5: golden user_data_payload_byte [1528+8] = 57 | ovt user_data_payload_byte [1528+8] = 49

12/36 Test #1380: syntax_golden_h265_trace_headers_syn_pat_hlg_patches ..........   Passed    0.11 sec
      Start 1381: syntax_golden_h265_trace_headers_syn_pat_p3_saturation
      Start 1382: syntax_golden_h265_trace_headers_syn_pat_pq_patches_1000
13/36 Test #1377: syntax_golden_h265_trace_headers_syn_h265_pq_colr_only ........***Failed    0.24 sec
syn_h265_pq_colr_only.mp4: 10 units parsed, 2758 golden elements, 2758 OVT elements
unit 0: golden user_data_payload_byte [1488+8] = 49 | ovt user_data_payload_byte [1488+8] = 48
unit 0: golden user_data_payload_byte [1496+8] = 49 | ovt user_data_payload_byte [1496+8] = 52
unit 0: golden user_data_payload_byte [1504+8] = 49 | ovt user_data_payload_byte [1504+8] = 57
unit 0: golden user_data_payload_byte [1520+8] = 51 | ovt user_data_payload_byte [1520+8] = 55
unit 0: golden user_data_payload_byte [1528+8] = 57 | ovt user_data_payload_byte [1528+8] = 49

      Start 1383: syntax_golden_h265_trace_headers_syn_pat_sdr709_ramp
14/36 Test #1379: syntax_golden_h265_trace_headers_syn_h265_sei_unknown_200 .....***Failed    0.20 sec
syn_h265_sei_unknown_200.265: 11 units parsed, 2777 golden elements, 2777 OVT elements
unit 3: golden user_data_payload_byte [1576+8] = 49 | ovt user_data_payload_byte [1576+8] = 48
unit 3: golden user_data_payload_byte [1584+8] = 49 | ovt user_data_payload_byte [1584+8] = 52
unit 3: golden user_data_payload_byte [1592+8] = 49 | ovt user_data_payload_byte [1592+8] = 57
unit 3: golden user_data_payload_byte [1608+8] = 51 | ovt user_data_payload_byte [1608+8] = 55
unit 3: golden user_data_payload_byte [1616+8] = 57 | ovt user_data_payload_byte [1616+8] = 49

      Start 1384: syntax_golden_h265_trace_headers_syn_pat_siting_checker
15/36 Test #1382: syntax_golden_h265_trace_headers_syn_pat_pq_patches_1000 ......   Passed    0.07 sec
      Start 1429: syntax_golden_h265_gstparse_syn_h265_amp_sao_on
16/36 Test #1381: syntax_golden_h265_trace_headers_syn_pat_p3_saturation ........   Passed    0.09 sec
      Start 1430: syntax_golden_h265_gstparse_syn_h265_bpyramid
17/36 Test #1383: syntax_golden_h265_trace_headers_syn_pat_sdr709_ramp ..........   Passed    0.07 sec
      Start 1431: syntax_golden_h265_gstparse_syn_h265_crop_1080_in_1088
18/36 Test #1384: syntax_golden_h265_trace_headers_syn_pat_siting_checker .......   Passed    0.07 sec
      Start 1432: syntax_golden_h265_gstparse_syn_h265_gop60_1080p
19/36 Test #1430: syntax_golden_h265_gstparse_syn_h265_bpyramid .................   Passed    0.10 sec
      Start 1433: syntax_golden_h265_gstparse_syn_h265_hdr10plus
20/36 Test #1429: syntax_golden_h265_gstparse_syn_h265_amp_sao_on ...............   Passed    0.12 sec
      Start 1434: syntax_golden_h265_gstparse_syn_h265_hrd_cbr
21/36 Test #1431: syntax_golden_h265_gstparse_syn_h265_crop_1080_in_1088 ........   Passed    0.12 sec
      Start 1435: syntax_golden_h265_gstparse_syn_h265_hrd_vbr
22/36 Test #1433: syntax_golden_h265_gstparse_syn_h265_hdr10plus ................   Passed    0.16 sec
      Start 1436: syntax_golden_h265_gstparse_syn_h265_main10_hev1
23/36 Test #1434: syntax_golden_h265_gstparse_syn_h265_hrd_cbr ..................   Passed    0.20 sec
      Start 1437: syntax_golden_h265_gstparse_syn_h265_main10_pq_hdr
24/36 Test #1432: syntax_golden_h265_gstparse_syn_h265_gop60_1080p ..............   Passed    0.31 sec
      Start 1438: syntax_golden_h265_gstparse_syn_h265_multi_pps
25/36 Test #1435: syntax_golden_h265_gstparse_syn_h265_hrd_vbr ..................   Passed    0.23 sec
      Start 1439: syntax_golden_h265_gstparse_syn_h265_pq_colr_only
26/36 Test #1436: syntax_golden_h265_gstparse_syn_h265_main10_hev1 ..............   Passed    0.19 sec
      Start 1440: syntax_golden_h265_gstparse_syn_h265_pq_sei_only
27/36 Test #1437: syntax_golden_h265_gstparse_syn_h265_main10_pq_hdr ............   Passed    0.17 sec
      Start 1441: syntax_golden_h265_gstparse_syn_h265_sei_unknown_200
28/36 Test #1438: syntax_golden_h265_gstparse_syn_h265_multi_pps ................   Passed    0.15 sec
      Start 1442: syntax_golden_h265_gstparse_syn_pat_hlg_patches
29/36 Test #1439: syntax_golden_h265_gstparse_syn_h265_pq_colr_only .............   Passed    0.13 sec
      Start 1443: syntax_golden_h265_gstparse_syn_pat_p3_saturation
30/36 Test #1440: syntax_golden_h265_gstparse_syn_h265_pq_sei_only ..............   Passed    0.12 sec
      Start 1444: syntax_golden_h265_gstparse_syn_pat_pq_patches_1000
31/36 Test #1441: syntax_golden_h265_gstparse_syn_h265_sei_unknown_200 ..........   Passed    0.16 sec
      Start 1445: syntax_golden_h265_gstparse_syn_pat_sdr709_ramp
32/36 Test #1442: syntax_golden_h265_gstparse_syn_pat_hlg_patches ...............   Passed    0.13 sec
      Start 1446: syntax_golden_h265_gstparse_syn_pat_siting_checker
33/36 Test #1443: syntax_golden_h265_gstparse_syn_pat_p3_saturation .............   Passed    0.14 sec
34/36 Test #1444: syntax_golden_h265_gstparse_syn_pat_pq_patches_1000 ...........   Passed    0.12 sec
35/36 Test #1446: syntax_golden_h265_gstparse_syn_pat_siting_checker ............   Passed    0.11 sec
36/36 Test #1445: syntax_golden_h265_gstparse_syn_pat_sdr709_ramp ...............   Passed    0.12 sec

69% tests passed, 11 tests failed out of 36

Label Time Summary:
fast             =   5.91 sec*proc (36 tests)
golden           =   5.91 sec*proc (36 tests)
needs_corpus     =   5.91 sec*proc (36 tests)
syntax_golden    =   5.91 sec*proc (36 tests)

Total Test time (real) =   4.09 sec

The following tests FAILED:
    1367 - syntax_golden_h265_trace_headers_syn_h265_amp_sao_on (Failed) fast golden needs_corpus syntax_golden
    1370 - syntax_golden_h265_trace_headers_syn_h265_gop60_1080p (Failed) fast golden needs_corpus syntax_golden
    1371 - syntax_golden_h265_trace_headers_syn_h265_hdr10plus (Failed) fast golden needs_corpus syntax_golden
    1372 - syntax_golden_h265_trace_headers_syn_h265_hrd_cbr (Failed) fast golden needs_corpus syntax_golden
    1373 - syntax_golden_h265_trace_headers_syn_h265_hrd_vbr (Failed) fast golden needs_corpus syntax_golden
    1374 - syntax_golden_h265_trace_headers_syn_h265_main10_hev1 (Failed) fast golden needs_corpus syntax_golden
    1375 - syntax_golden_h265_trace_headers_syn_h265_main10_pq_hdr (Failed) fast golden needs_corpus syntax_golden
    1376 - syntax_golden_h265_trace_headers_syn_h265_multi_pps (Failed) fast golden needs_corpus syntax_golden
    1377 - syntax_golden_h265_trace_headers_syn_h265_pq_colr_only (Failed) fast golden needs_corpus syntax_golden
    1378 - syntax_golden_h265_trace_headers_syn_h265_pq_sei_only (Failed) fast golden needs_corpus syntax_golden
    1379 - syntax_golden_h265_trace_headers_syn_h265_sei_unknown_200 (Failed) fast golden needs_corpus syntax_golden
Errors while running CTest
```
